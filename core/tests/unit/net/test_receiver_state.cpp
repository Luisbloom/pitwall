// Pruebas del estado de la adquisicion (F1.1-10, RF-012).
//
// Todas con reloj manual: el receptor lee la hora de una funcion inyectada y la
// prueba la mueve a mano. Ninguna duerme los 2 s del silencio; lo unico que se
// espera es a que el bucle de T1 de un par de vueltas, que con el socket de
// guion es cuestion de milisegundos.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pw/config/config.hpp"
#include "pw/mem/buffer_pool.hpp"
#include "pw/mem/spsc_ring.hpp"
#include "pw/net/udp_receiver.hpp"
#include "pw/obs/logging.hpp"
#include "pw/obs/metrics.hpp"
#include "deterministic_priority.hpp"
#include "scripted_socket.hpp"

using namespace std::chrono_literals;

using pw::net::Ipv4Endpoint;
using pw::net::ReceivedDatagram;
using pw::net::SocketErrorKind;
using pw::net::UdpReceiverState;
using pw::test::ScriptedDatagram;
using pw::test::scriptedError;
using pw::test::ScriptedSocket;

namespace {

constexpr std::uint64_t kMs = 1'000'000ULL;

// ── Reloj manual ────────────────────────────────────────────────────────────

std::atomic<std::uint64_t> manualNowNs{0};

std::uint64_t manualClock() noexcept { return manualNowNs.load(std::memory_order_acquire); }

void setClockMs(const std::uint64_t ms) { manualNowNs.store(ms * kMs, std::memory_order_release); }

template <typename Predicate>
bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout = 5000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) { return true; }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

constexpr Ipv4Endpoint kGame{{192, 168, 1, 50}, 20777};

ScriptedDatagram datagram(const std::size_t size = 64) { return ScriptedDatagram{kGame, size}; }

class MemorySink final : public pw::obs::ILogSink {
public:
    void write(const std::string_view line) override
    {
        const std::lock_guard lock{mutex_};
        lines_.emplace_back(line);
    }
    void flush() override {}

    [[nodiscard]] std::size_t countContaining(const std::string_view needle) const
    {
        const std::lock_guard lock{mutex_};
        std::size_t           found = 0;
        for (const auto& line : lines_) {
            if (line.find(needle) != std::string::npos) { ++found; }
        }
        return found;
    }

private:
    mutable std::mutex       mutex_;
    std::vector<std::string> lines_;
};

using Queue    = pw::mem::SpscRing<ReceivedDatagram, 64>;
using Receiver = pw::net::UdpReceiverT<Queue, Queue>;

/// Receptor con socket de guion y reloj manual, corriendo en su propio hilo.
///
/// El guion se entrega por tramos con deliver(): el socket no suelta mas
/// entradas de las permitidas y, mientras tanto, devuelve esperas vencidas, que
/// es exactamente lo que hace un socket real cuando el juego no emite.
class StateRig {
public:
    explicit StateRig(std::vector<ScriptedDatagram> script, const std::uint64_t startMs = 1'000)
        : pool_{64, 512}
        , raw_{std::make_unique<Queue>()}
        , rec_{std::make_unique<Queue>()}
        , logger_{loggerConfig()}
        , sink_{std::make_shared<MemorySink>()}
    {
        setClockMs(startMs);
        logger_.addSink(sink_);

        pw::net::UdpReceiverConfig config;
        config.silenceTimeoutMs       = 2000;
        config.poolExhaustedBackoffUs = 50;

        auto scripted = std::make_unique<ScriptedSocket>(script);
        socket_       = scripted.get();
        socket_->allowUpTo(0);
        receiver_ = std::make_unique<Receiver>(config, pool_, *raw_, *rec_, metrics_, logger_,
                                               std::move(scripted), &manualClock,
                                               &pw::test::fakePriorityRaised);
        REQUIRE(receiver_->open().has_value());
        worker_ = std::thread{[this] { receiver_->run(stopper_.get_token()); }};
    }

    ~StateRig()
    {
        stopper_.request_stop();
        if (worker_.joinable()) { worker_.join(); }
        drain();
    }

    StateRig(const StateRig&)            = delete;
    StateRig& operator=(const StateRig&) = delete;

    /// Entrega las `n` entradas siguientes del guion y espera a que T1 las haya
    /// procesado todas.
    void deliver(const std::size_t n = 1)
    {
        const auto target = socket_->delivered() + n;
        socket_->allowUpTo(target);
        REQUIRE(waitUntil([&] { return socket_->delivered() == target; }));
        settle();
        drain();
    }

    /// Mueve el reloj y espera a que T1 lo haya mirado.
    void advanceToMs(const std::uint64_t ms)
    {
        setClockMs(ms);
        settle();
    }

    /// Espera dos esperas vencidas completas: la segunda empieza despues de que
    /// la primera haya terminado, asi que al menos una comprobacion del silencio
    /// ha corrido entera con el estado actual del reloj y del guion.
    void settle()
    {
        const auto from = socket_->idlePolls();
        REQUIRE(waitUntil([&] { return socket_->idlePolls() >= from + 2; }));
    }

    void drain()
    {
        ReceivedDatagram item;
        while (raw_->tryPop(item)) { pool_.release(item.handle); }
        while (rec_->tryPop(item)) { pool_.release(item.handle); }
    }

    [[nodiscard]] Receiver&                 receiver() noexcept { return *receiver_; }
    [[nodiscard]] pw::obs::MetricsRegistry& metrics() noexcept { return metrics_; }
    [[nodiscard]] pw::obs::Logger&          logger() noexcept { return logger_; }
    [[nodiscard]] MemorySink&               sink() noexcept { return *sink_; }

    [[nodiscard]] std::int64_t stateGauge()
    {
        return metrics_.gauge("udp_receiver_state").value();
    }
    [[nodiscard]] std::uint64_t silenceCounter()
    {
        return metrics_.counter("udp_silence_transitions_total").value();
    }

private:
    static pw::obs::LoggerConfig loggerConfig()
    {
        pw::obs::LoggerConfig config;
        config.level               = pw::obs::LogLevel::Info;
        config.aggregationWindowMs = 0;
        return config;
    }

    pw::mem::BufferPool                 pool_;
    std::unique_ptr<Queue>              raw_;
    std::unique_ptr<Queue>              rec_;
    pw::obs::MetricsRegistry            metrics_;
    pw::obs::Logger                     logger_;
    std::shared_ptr<MemorySink>         sink_;
    ScriptedSocket*                     socket_{nullptr};
    std::unique_ptr<Receiver>           receiver_;
    std::stop_source                    stopper_;
    std::thread                         worker_;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Listening
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("El estado inicial es Listening", "[net][receiver][state][RF-012][F1.1-10]")
{
    StateRig rig{{datagram()}};

    REQUIRE(rig.receiver().state() == UdpReceiverState::Listening);
    REQUIRE(rig.stateGauge() == 0);
    REQUIRE(rig.receiver().lastActivityNs() == 0);
    REQUIRE(rig.receiver().receivingTransitions() == 0);
    REQUIRE(rig.receiver().silenceTransitions() == 0);
}

TEST_CASE("Sin datagramas, ni las esperas vencidas ni el tiempo sacan de Listening",
          "[net][receiver][state][RF-012][F1.1-10]")
{
    // «Nunca ha llegado nada» no es «ha dejado de llegar»: el silencio no
    // convierte Listening en Silent, por mucho que pase.
    StateRig rig{{datagram()}};

    rig.settle();   // varias esperas vencidas
    REQUIRE(rig.receiver().state() == UdpReceiverState::Listening);

    rig.advanceToMs(1'000 + 60'000);   // un minuto despues
    REQUIRE(rig.receiver().state() == UdpReceiverState::Listening);
    REQUIRE(rig.receiver().silenceTransitions() == 0);
    REQUIRE(rig.receiver().receivingTransitions() == 0);
    REQUIRE(rig.receiver().silenceTransitions() == 0);
    REQUIRE(rig.silenceCounter() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Receiving y el limite exacto
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("El primer datagrama pasa a Receiving; los siguientes no son transiciones",
          "[net][receiver][state][RF-012][F1.1-10]")
{
    StateRig rig{{datagram(), datagram(), datagram(), datagram(), datagram()}};

    rig.deliver(1);
    REQUIRE(rig.receiver().state() == UdpReceiverState::Receiving);
    REQUIRE(rig.stateGauge() == 1);
    REQUIRE(rig.receiver().receivingTransitions() == 1);
    REQUIRE(rig.receiver().silenceTransitions() == 0);
    REQUIRE(rig.receiver().lastActivityNs() == 1'000 * kMs);

    rig.advanceToMs(1'500);
    rig.deliver(4);
    REQUIRE(rig.receiver().state() == UdpReceiverState::Receiving);
    REQUIRE(rig.receiver().receivingTransitions() == 1);   // Receiving -> Receiving no cuenta
    REQUIRE(rig.receiver().silenceTransitions() == 0);
    REQUIRE(rig.receiver().lastActivityNs() == 1'500 * kMs);
}

TEST_CASE("El silencio vence al ALCANZAR silenceTimeoutMs: elapsed >= timeout",
          "[net][receiver][state][RF-012][F1.1-10]")
{
    StateRig rig{{datagram()}};
    rig.deliver(1);   // actividad en t = 1000 ms

    // Un milisegundo antes del limite: sigue recibiendo.
    rig.advanceToMs(1'000 + 1'999);
    REQUIRE(rig.receiver().state() == UdpReceiverState::Receiving);
    REQUIRE(rig.receiver().silenceTransitions() == 0);

    // Exactamente en el limite: silencio.
    rig.advanceToMs(1'000 + 2'000);
    REQUIRE(rig.receiver().state() == UdpReceiverState::Silent);
    REQUIRE(rig.stateGauge() == 2);
    REQUIRE(rig.receiver().silenceTransitions() == 1);
    REQUIRE(rig.silenceCounter() == 1);
    REQUIRE(rig.receiver().receivingTransitions() == 1);
}

TEST_CASE("Pasado el limite, mas esperas vencidas no repiten la transicion",
          "[net][receiver][state][RF-012][F1.1-10]")
{
    StateRig rig{{datagram()}};
    rig.deliver(1);
    rig.advanceToMs(1'000 + 2'500);   // superado
    REQUIRE(rig.receiver().state() == UdpReceiverState::Silent);

    for (std::uint64_t ms = 4'000; ms <= 20'000; ms += 4'000) { rig.advanceToMs(ms); }

    REQUIRE(rig.receiver().state() == UdpReceiverState::Silent);
    REQUIRE(rig.receiver().silenceTransitions() == 1);
    REQUIRE(rig.silenceCounter() == 1);
    REQUIRE(rig.receiver().receivingTransitions() == 1);
}

TEST_CASE("Un datagrama nuevo sale de Silent, y un segundo silencio cuenta dos",
          "[net][receiver][state][RF-012][F1.1-10]")
{
    StateRig rig{{datagram(), datagram()}};

    rig.deliver(1);                    // Listening -> Receiving (t = 1000)
    rig.advanceToMs(3'000);            // Receiving -> Silent
    REQUIRE(rig.receiver().state() == UdpReceiverState::Silent);

    rig.advanceToMs(10'000);
    rig.deliver(1);                    // Silent -> Receiving (t = 10000)
    REQUIRE(rig.receiver().state() == UdpReceiverState::Receiving);
    REQUIRE(rig.stateGauge() == 1);
    REQUIRE(rig.receiver().lastActivityNs() == 10'000 * kMs);
    REQUIRE(rig.receiver().silenceTransitions() == 1);   // volver no es un silencio

    rig.advanceToMs(11'999);
    REQUIRE(rig.receiver().state() == UdpReceiverState::Receiving);
    rig.advanceToMs(12'000);           // Receiving -> Silent, otra vez
    REQUIRE(rig.receiver().state() == UdpReceiverState::Silent);
    REQUIRE(rig.receiver().silenceTransitions() == 2);
    REQUIRE(rig.silenceCounter() == 2);
    REQUIRE(rig.receiver().receivingTransitions() == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Que renueva la actividad y que no
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Un truncado renueva la actividad", "[net][receiver][state][RF-012][RF-004][F1.1-10]")
{
    StateRig rig{{datagram(), datagram(10)}};

    rig.deliver(1);                    // t = 1000
    rig.advanceToMs(2'500);
    rig.deliver(1);                    // truncado en t = 2500
    REQUIRE(rig.receiver().stats().truncated == 1);
    REQUIRE(rig.receiver().lastActivityNs() == 2'500 * kMs);

    rig.advanceToMs(4'000);            // 3000 desde el primero, 1500 desde el truncado
    REQUIRE(rig.receiver().state() == UdpReceiverState::Receiving);
    rig.advanceToMs(4'500);            // 2000 desde el truncado
    REQUIRE(rig.receiver().state() == UdpReceiverState::Silent);
}

TEST_CASE("Un oversized renueva la actividad y tambien saca de Silent",
          "[net][receiver][state][RF-012][F1.1-10]")
{
    // WSAEMSGSIZE / EMSGSIZE solo aparecen si ha llegado un datagrama. No hay
    // origen ni contenido, pero hay trafico: la actividad global se renueva.
    StateRig rig{{datagram(), datagram(600), datagram(600)}};   // buffer de 512 B

    rig.deliver(1);                    // t = 1000
    rig.advanceToMs(2'500);
    rig.deliver(1);                    // oversized en t = 2500
    REQUIRE(rig.receiver().stats().oversized == 1);
    REQUIRE(rig.receiver().lastActivityNs() == 2'500 * kMs);
    rig.advanceToMs(4'000);
    REQUIRE(rig.receiver().state() == UdpReceiverState::Receiving);

    rig.advanceToMs(4'500);
    REQUIRE(rig.receiver().state() == UdpReceiverState::Silent);
    rig.deliver(1);                    // otro oversized: Silent -> Receiving
    REQUIRE(rig.receiver().state() == UdpReceiverState::Receiving);
}

TEST_CASE("Un fallo del socket no renueva la actividad ni se confunde con silencio",
          "[net][receiver][state][RF-012][F1.1-10]")
{
    StateRig rig{{datagram(), scriptedError(SocketErrorKind::Unknown),
                  scriptedError(SocketErrorKind::Unknown)}};

    rig.deliver(1);                    // t = 1000
    rig.advanceToMs(2'000);
    rig.deliver(1);                    // fallo en t = 2000
    REQUIRE(rig.receiver().stats().socketErrors == 1);
    REQUIRE(rig.receiver().lastActivityNs() == 1'000 * kMs);   // no ha renovado
    REQUIRE(rig.receiver().state() == UdpReceiverState::Receiving);   // ni ha silenciado

    // El silencio se sigue midiendo desde el ultimo DATAGRAMA.
    rig.advanceToMs(3'000);
    REQUIRE(rig.receiver().state() == UdpReceiverState::Silent);

    rig.deliver(1);                    // otro fallo con Silent: no reconecta
    REQUIRE(rig.receiver().state() == UdpReceiverState::Silent);
    REQUIRE(rig.receiver().silenceTransitions() == 1);
}

namespace {

std::atomic<std::uint64_t> steppingNowNs{0};

/// Reloj que avanza 3 s en cada lectura: cualquier comprobacion de silencio
/// que se colara lo detectaria.
std::uint64_t steppingClock() noexcept
{
    return steppingNowNs.fetch_add(3'000 * kMs, std::memory_order_acq_rel) + 3'000 * kMs;
}

} // namespace

TEST_CASE("El cierre del socket termina el bucle y no se confunde con silencio",
          "[net][receiver][state][RF-012][F1.1-10]")
{
    pw::mem::BufferPool      pool{16, 512};
    auto                     raw = std::make_unique<Queue>();
    auto                     rec = std::make_unique<Queue>();
    pw::obs::MetricsRegistry metrics;
    pw::obs::Logger          logger{pw::obs::LoggerConfig{}};

    const std::vector<ScriptedDatagram> script{datagram(), scriptedError(SocketErrorKind::Closed)};
    Receiver receiver{pw::net::UdpReceiverConfig{}, pool, *raw, *rec, metrics, logger,
                      std::make_unique<ScriptedSocket>(script), &steppingClock,
                      &pw::test::fakePriorityRaised};
    REQUIRE(receiver.open().has_value());

    std::stop_source never;
    receiver.run(never.get_token());   // vuelve por el cierre, en este mismo hilo

    // Entre el datagrama y el cierre el reloj ha saltado 3 s, mas que el
    // silencio. Aun asi, el cierre no es silencio: el estado se queda.
    REQUIRE(receiver.eventSeen(pw::net::ReceiverEvent::SocketClosed));
    REQUIRE(receiver.state() == UdpReceiverState::Receiving);
    REQUIRE(receiver.silenceTransitions() == 0);

    ReceivedDatagram item;
    while (raw->tryPop(item)) { pool.release(item.handle); }
    while (rec->tryPop(item)) { pool.release(item.handle); }
}

// ─────────────────────────────────────────────────────────────────────────────
// Registro fuera de T1
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Compara un informe con lo esperado, con un mensaje legible si falla.
void requireReport(const pw::net::StateTransitionReport& report, const std::uint64_t toReceiving,
                   const std::uint64_t toSilent)
{
    INFO("informe: " << report.toReceiving << " a Receiving, " << report.toSilent << " a Silent");
    REQUIRE(report.toReceiving == toReceiving);
    REQUIRE(report.toSilent == toSilent);
}

} // namespace

TEST_CASE("Transiciones A-C: cada entrada se informa una vez y con su tipo",
          "[net][receiver][state][transitions][RF-012][F1.1-10]")
{
    StateRig rig{{datagram(), datagram()}};
    auto&    receiver = rig.receiver();

    requireReport(receiver.reportStateTransitions(), 0, 0);

    // A) Listening -> Receiving: 1 a Receiving, 0 a Silent.
    rig.deliver(1);
    rig.logger().flush();
    REQUIRE(rig.sink().countContaining("recibiendo datagramas UDP") == 0);   // T1 no registra
    requireReport(receiver.reportStateTransitions(), 1, 0);
    requireReport(receiver.reportStateTransitions(), 0, 0);                  // ya informado
    rig.logger().flush();
    REQUIRE(rig.sink().countContaining("recibiendo datagramas UDP") == 1);

    // B) Receiving -> Silent: +1 a Silent.
    rig.advanceToMs(3'000);
    requireReport(receiver.reportStateTransitions(), 0, 1);
    rig.logger().flush();
    REQUIRE(rig.sink().countContaining("sin datagramas UDP") == 1);

    // C) Silent -> Receiving: +1 a Receiving.
    rig.deliver(1);
    requireReport(receiver.reportStateTransitions(), 1, 0);
    rig.logger().flush();
    REQUIRE(rig.sink().countContaining("recibiendo datagramas UDP") == 2);

    REQUIRE(receiver.receivingTransitions() == 2);
    REQUIRE(receiver.silenceTransitions() == 1);
    REQUIRE(rig.silenceCounter() == 1);
}

TEST_CASE("Transiciones D: Receiving -> Silent -> Receiving entre dos lecturas da 1 y 1",
          "[net][receiver][state][transitions][RF-012][F1.1-10]")
{
    StateRig rig{{datagram(), datagram(), datagram()}};
    auto&    receiver = rig.receiver();

    rig.deliver(1);                                             // t = 1000
    requireReport(receiver.reportStateTransitions(), 1, 0);    // lectura 1

    // Entre dos lecturas: silencio y vuelta. El estado final es el mismo que
    // el de la lectura anterior, pero no se ha perdido nada de lo que paso.
    rig.advanceToMs(3'000);                                     // -> Silent
    rig.advanceToMs(4'000);
    rig.deliver(1);                                             // -> Receiving
    REQUIRE(receiver.state() == UdpReceiverState::Receiving);

    const auto report = receiver.reportStateTransitions();     // lectura 2
    requireReport(report, 1, 1);
    rig.logger().flush();
    REQUIRE(rig.sink().countContaining("sin datagramas UDP") == 1);
    REQUIRE(rig.sink().countContaining("entradasEnSilencio") == 1);

    // Y el caso simetrico: Silent -> Receiving -> Silent entre lecturas.
    rig.advanceToMs(6'000);                                     // -> Silent
    requireReport(receiver.reportStateTransitions(), 0, 1);
    rig.advanceToMs(7'000);
    rig.deliver(1);                                             // -> Receiving (t = 7000)
    rig.advanceToMs(9'000);                                     // -> Silent
    REQUIRE(receiver.state() == UdpReceiverState::Silent);
    requireReport(receiver.reportStateTransitions(), 1, 1);

    REQUIRE(rig.silenceCounter() == 3);   // la metrica sigue contando SOLO Receiving -> Silent
}

TEST_CASE("Transiciones E-F: esperas en Silent y datagramas en Receiving no suman",
          "[net][receiver][state][transitions][RF-012][F1.1-10]")
{
    std::vector<ScriptedDatagram> script(20, datagram());
    StateRig                      rig{script};
    auto&                         receiver = rig.receiver();

    rig.deliver(1);
    requireReport(receiver.reportStateTransitions(), 1, 0);

    // F) Varios datagramas ya en Receiving: ninguna transicion.
    rig.deliver(10);
    requireReport(receiver.reportStateTransitions(), 0, 0);

    rig.advanceToMs(3'000);
    requireReport(receiver.reportStateTransitions(), 0, 1);

    // E) Varias esperas ya en Silent, con el reloj avanzando: ninguna transicion.
    for (std::uint64_t ms = 4'000; ms <= 12'000; ms += 2'000) { rig.advanceToMs(ms); }
    requireReport(receiver.reportStateTransitions(), 0, 0);

    REQUIRE(receiver.receivingTransitions() == 1);
    REQUIRE(receiver.silenceTransitions() == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Configuracion
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("El silencio por defecto es 2000 ms en el receptor y en la configuracion",
          "[net][receiver][state][config][RF-012][F1.1-10]")
{
    // Dos modulos que no deben conocerse comparten un numero: esta prueba es el
    // unico punto donde se encuentran.
    STATIC_REQUIRE(pw::net::kDefaultSilenceTimeoutMs == 2000);
    STATIC_REQUIRE(pw::config::kDefaultSilenceTimeoutMs == pw::net::kDefaultSilenceTimeoutMs);
    REQUIRE(pw::net::UdpReceiverConfig{}.silenceTimeoutMs == 2000);
    REQUIRE(pw::config::defaults().net.silenceTimeoutMs == 2000);
}

TEST_CASE("Un silencio configurado distinto se respeta", "[net][receiver][state][RF-012][F1.1-10]")
{
    pw::mem::BufferPool      pool{16, 512};
    auto                     raw = std::make_unique<Queue>();
    auto                     rec = std::make_unique<Queue>();
    pw::obs::MetricsRegistry metrics;
    pw::obs::Logger          logger{pw::obs::LoggerConfig{}};

    pw::net::UdpReceiverConfig config;
    config.silenceTimeoutMs = 1500;

    const std::vector<ScriptedDatagram> script{datagram()};
    auto  scripted = std::make_unique<ScriptedSocket>(script);
    auto* socket   = scripted.get();
    setClockMs(1'000);
    Receiver receiver{config,        pool,         *raw, *rec, metrics, logger, std::move(scripted),
                      &manualClock, &pw::test::fakePriorityRaised};
    REQUIRE(receiver.open().has_value());

    std::stop_source stopper;
    std::thread      worker{[&] { receiver.run(stopper.get_token()); }};
    REQUIRE(waitUntil([&] { return receiver.state() == UdpReceiverState::Receiving; }));

    const auto settle = [&] {
        const auto from = socket->idlePolls();
        REQUIRE(waitUntil([&] { return socket->idlePolls() >= from + 2; }));
    };
    setClockMs(1'000 + 1'499);
    settle();
    REQUIRE(receiver.state() == UdpReceiverState::Receiving);
    setClockMs(1'000 + 1'500);
    settle();
    REQUIRE(receiver.state() == UdpReceiverState::Silent);

    stopper.request_stop();
    worker.join();
    ReceivedDatagram item;
    while (raw->tryPop(item)) { pool.release(item.handle); }
    while (rec->tryPop(item)) { pool.release(item.handle); }
}
