// Pruebas de la parada del receptor (F1.1-12, RNF-055 en su parte de adquisicion).
//
// El contrato que se prueba:
//
//   controlador   request_stop()
//   T1            termina como mucho la vuelta en curso y vuelve de run()
//                 sin retener ninguna referencia a buffers
//   controlador   join() y DESPUES close() o destruccion
//   dueno         drena Qraw/Qrec, suelta cada handle, pool.inUse() == 0
//
// La latencia se mide en VUELTAS (llamadas a receive()), no en milisegundos:
// asi la prueba no depende del planificador. Ninguna duerme de mas.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

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
using pw::net::ReceiverEvent;
using pw::net::SocketErrorKind;
using pw::net::ThreadPriorityResult;
using pw::net::UdpReceiverState;
using pw::test::ScriptedDatagram;
using pw::test::scriptedError;
using pw::test::ScriptedSocket;

namespace {

constexpr std::uint64_t    kMs = 1'000'000ULL;
std::atomic<std::uint64_t> manualNowNs{1'000 * kMs};

std::uint64_t manualClock() noexcept { return manualNowNs.load(std::memory_order_acquire); }

/// Raiser que cuenta: F1.1-11 debe intentarse una vez por run(), tambien al parar.
std::atomic<int> raiserCalls{0};

ThreadPriorityResult countingRaiser() noexcept
{
    raiserCalls.fetch_add(1, std::memory_order_relaxed);
    return ThreadPriorityResult::Raised;
}

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

ScriptedDatagram datagram() { return ScriptedDatagram{kGame, 64}; }

/// Colas diminutas: se llenan con 4 elementos, asi una parada con colas llenas
/// y descartes es facil de montar.
using Queue    = pw::mem::SpscRing<ReceivedDatagram, 4>;
using Receiver = pw::net::UdpReceiverT<Queue, Queue>;

/// Receptor con socket de guion, reloj manual y raiser que cuenta. El hilo y
/// el stop_source son de la prueba, como lo seran del controlador real.
struct ShutdownRig {
    explicit ShutdownRig(const std::vector<ScriptedDatagram>& script)
        : pool{64, 512}
        , raw{std::make_unique<Queue>()}
        , rec{std::make_unique<Queue>()}
        , logger{pw::obs::LoggerConfig{}}
    {
        raiserCalls.store(0);
        manualNowNs.store(1'000 * kMs);

        pw::net::UdpReceiverConfig config;
        config.poolExhaustedBackoffUs = 50;

        auto scripted = std::make_unique<ScriptedSocket>(script);
        socket        = scripted.get();
        socket->allowUpTo(0);
        receiver = std::make_unique<Receiver>(config, pool, *raw, *rec, metrics, logger,
                                              std::move(scripted), &manualClock, &countingRaiser);
        REQUIRE(receiver->open().has_value());
    }

    /// SALVAGUARDA DE PRUEBA, no parte del contrato. Si una REQUIRE falla con
    /// T1 todavia en marcha, destruir un std::thread «joinable» llamaria a
    /// std::terminate y se perderia el informe de Catch2. Aqui se aplica el
    /// mismo orden que el contrato -parar y unir, y solo entonces soltar lo que
    /// quede en las colas- y NUNCA se cierra el socket: el receptor lo cierra
    /// al destruirse, despues de este cuerpo y con T1 ya unido. Las pruebas no
    /// dependen de esto: todas llaman a stopAndJoin() explicitamente.
    ~ShutdownRig()
    {
        if (worker.joinable()) {
            stopper.request_stop();
            worker.join();
        }
        drain();
    }

    ShutdownRig(const ShutdownRig&)            = delete;
    ShutdownRig& operator=(const ShutdownRig&) = delete;

    void start()
    {
        worker = std::thread{[this] {
            receiver->run(stopper.get_token());
            returned.store(true, std::memory_order_release);
        }};
    }

    /// Entrega las `n` entradas siguientes y espera a que T1 las haya procesado.
    void deliver(const std::size_t n)
    {
        const auto target = socket->delivered() + n;
        socket->allowUpTo(target);
        REQUIRE(waitUntil([&] { return socket->delivered() == target; }));
        settle();
    }

    void settle()
    {
        const auto from = socket->idlePolls();
        REQUIRE(waitUntil([&] { return socket->idlePolls() >= from + 2; }));
    }

    /// request_stop + join: el orden del controlador. Devuelve cuantas llamadas
    /// a receive() hizo T1 DESPUES de pedirle parar.
    std::uint64_t stopAndJoin()
    {
        stopper.request_stop();
        const auto callsAtStop = socket->receiveCalls();
        worker.join();
        return socket->receiveCalls() - callsAtStop;
    }

    /// El consumidor/dueno de las colas vacia y suelta. Devuelve cuantos
    /// elementos habia y cuantos buffers distintos.
    struct Drained {
        std::size_t raw{0};
        std::size_t rec{0};
        std::size_t distinctBuffers{0};
    };
    Drained drain()
    {
        Drained                 out;
        std::set<std::uint32_t> distinct;
        ReceivedDatagram        item;
        while (raw->tryPop(item)) {
            ++out.raw;
            distinct.insert(item.handle.index());
            pool.release(item.handle);
        }
        while (rec->tryPop(item)) {
            ++out.rec;
            distinct.insert(item.handle.index());
            pool.release(item.handle);
        }
        out.distinctBuffers = distinct.size();
        return out;
    }

    pw::mem::BufferPool       pool;
    std::unique_ptr<Queue>    raw;
    std::unique_ptr<Queue>    rec;
    pw::obs::MetricsRegistry  metrics;
    pw::obs::Logger           logger;
    ScriptedSocket*           socket{nullptr};
    std::unique_ptr<Receiver> receiver;
    std::stop_source          stopper;
    std::thread               worker;
    std::atomic<bool>         returned{false};
};

void requireNoShutdownEvents(const Receiver& receiver)
{
    REQUIRE_FALSE(receiver.eventSeen(ReceiverEvent::SocketClosed));
    REQUIRE_FALSE(receiver.eventSeen(ReceiverEvent::SocketFailure));
    REQUIRE(receiver.stats().socketErrors == 0);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// La parada
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Parada pedida antes de run(): vuelve sin recibir y sin sucesos",
          "[net][receiver][shutdown][F1.1-12]")
{
    ShutdownRig rig{{datagram()}};
    rig.socket->allowUpTo(1);

    rig.stopper.request_stop();
    rig.receiver->run(rig.stopper.get_token());   // en este mismo hilo: tiene que volver

    REQUIRE(rig.socket->receiveCalls() == 0);     // ni una vuelta
    REQUIRE(raiserCalls.load() == 1);             // F1.1-11 se intenta igual, una vez
    REQUIRE(rig.receiver->state() == UdpReceiverState::Listening);
    requireNoShutdownEvents(*rig.receiver);
    REQUIRE(rig.pool.inUse() == 0);
}

TEST_CASE("Parada esperando sin datagramas: vuelve en como mucho una espera mas",
          "[net][receiver][shutdown][F1.1-12]")
{
    ShutdownRig rig{{datagram()}};
    rig.start();
    rig.settle();   // T1 esta dando vueltas de espera vencida

    const auto extraCalls = rig.stopAndJoin();

    // Cuando la peticion llega, T1 esta dentro de una espera o entre dos. En el
    // peor caso empieza UNA mas antes de ver la senal. Nunca dos.
    REQUIRE(extraCalls <= 1);
    REQUIRE(rig.returned.load());
    REQUIRE(rig.receiver->state() == UdpReceiverState::Listening);   // F1.1-10 intacta
    REQUIRE(raiserCalls.load() == 1);                                 // F1.1-11 intacta
    requireNoShutdownEvents(*rig.receiver);
}

TEST_CASE("Parada despues de recibir: contadores y estado se conservan",
          "[net][receiver][shutdown][F1.1-12]")
{
    ShutdownRig rig{{datagram(), datagram(), datagram()}};
    rig.start();
    rig.deliver(3);
    REQUIRE(rig.receiver->state() == UdpReceiverState::Receiving);

    REQUIRE(rig.stopAndJoin() <= 1);

    REQUIRE(rig.receiver->stats().received == 3);
    REQUIRE(rig.receiver->state() == UdpReceiverState::Receiving);   // parar no es silencio
    REQUIRE(rig.receiver->silenceTransitions() == 0);
    requireNoShutdownEvents(*rig.receiver);

    rig.drain();
    REQUIRE(rig.pool.inUse() == 0);
}

TEST_CASE("Parada con Qraw y Qrec llenas: T1 no retiene nada y el dueno drena despues",
          "[net][receiver][shutdown][F1.1-12][RNF-055]")
{
    // 10 datagramas contra colas de 4 que nadie vacia: 4 encolados en cada una
    // (los mismos 4 buffers) y 6 descartes por cola.
    ShutdownRig rig{std::vector<ScriptedDatagram>(10, datagram())};
    rig.start();
    rig.deliver(10);
    REQUIRE(rig.receiver->stats().qrawDropped == 6);
    REQUIRE(rig.receiver->stats().qrecDropped == 6);

    REQUIRE(rig.stopAndJoin() <= 1);

    // T1 ya no existe. Lo que sigue en uso es EXACTAMENTE lo que hay en las
    // colas: ni una referencia de T1, ni una fugada por los descartes.
    const auto inUseAfterJoin = rig.pool.inUse();

    // El socket se cierra despues del join, y cerrar no genera ningun suceso.
    rig.receiver->close();
    REQUIRE_FALSE(rig.socket->isOpen());
    REQUIRE(rig.socket->closesDuringReceive() == 0);
    requireNoShutdownEvents(*rig.receiver);

    // Las colas siguen siendo validas tras parar el productor: su dueno drena.
    const auto drained = rig.drain();
    REQUIRE(drained.raw == 4);
    REQUIRE(drained.rec == 4);
    REQUIRE(drained.distinctBuffers == 4);
    REQUIRE(inUseAfterJoin == drained.distinctBuffers);
    REQUIRE(rig.pool.inUse() == 0);   // ahora si se puede destruir el pool
}

TEST_CASE("Tras la parada y el cierre no se recibe nada mas",
          "[net][receiver][shutdown][F1.1-12]")
{
    ShutdownRig rig{{datagram(), datagram(), datagram()}};
    rig.start();
    rig.deliver(1);
    rig.stopAndJoin();
    rig.receiver->close();

    // El guion permite ahora dos mas, pero no queda nadie para recibirlos.
    rig.socket->allowUpTo(3);
    REQUIRE(rig.receiver->stats().received == 1);
    REQUIRE(rig.socket->delivered() == 1);
    rig.drain();
    REQUIRE(rig.pool.inUse() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Parada frente a cierre, fallo e interrupcion
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Un socket cerrado antes de run() sigue siendo SocketClosed, no una parada",
          "[net][receiver][shutdown][F1.1-12]")
{
    ShutdownRig rig{{datagram()}};
    rig.receiver->close();   // antes de arrancar el hilo: sin concurrencia

    std::stop_source never;
    rig.receiver->run(never.get_token());   // vuelve por el cierre, no por una senal

    REQUIRE(rig.receiver->eventSeen(ReceiverEvent::SocketClosed));
    REQUIRE_FALSE(rig.receiver->eventSeen(ReceiverEvent::SocketFailure));
    REQUIRE(rig.receiver->stats().socketErrors == 0);
    REQUIRE(rig.socket->receiveCalls() == 1);
}

TEST_CASE("Un fallo real del socket sigue siendo SocketFailure, y la parada lo termina",
          "[net][receiver][shutdown][F1.1-12]")
{
    ShutdownRig rig{{scriptedError(SocketErrorKind::Unknown)}};
    rig.start();
    rig.deliver(1);

    REQUIRE(rig.receiver->stats().socketErrors == 1);
    REQUIRE(rig.receiver->eventSeen(ReceiverEvent::SocketFailure));

    REQUIRE(rig.stopAndJoin() <= 1);
    REQUIRE_FALSE(rig.receiver->eventSeen(ReceiverEvent::SocketClosed));
}

TEST_CASE("Interrupted no es un fallo, no es actividad y no termina el bucle",
          "[net][receiver][shutdown][F1.1-12]")
{
    ShutdownRig rig{{datagram(), scriptedError(SocketErrorKind::Interrupted),
                     scriptedError(SocketErrorKind::Interrupted),
                     scriptedError(SocketErrorKind::Interrupted)}};
    rig.start();
    rig.deliver(1);   // actividad en t = 1000
    manualNowNs.store(1'500 * kMs);
    rig.deliver(3);   // tres interrupciones en t = 1500

    REQUIRE(rig.receiver->stats().socketErrors == 0);
    REQUIRE_FALSE(rig.receiver->eventSeen(ReceiverEvent::SocketFailure));
    REQUIRE(rig.receiver->lastActivityNs() == 1'000 * kMs);   // no ha renovado
    REQUIRE(rig.receiver->state() == UdpReceiverState::Receiving);
    REQUIRE_FALSE(rig.returned.load());                        // sigue en el bucle

    // Y el silencio se sigue midiendo igual (F1.1-10).
    manualNowNs.store(3'000 * kMs);
    rig.settle();
    REQUIRE(rig.receiver->state() == UdpReceiverState::Silent);

    // Una parada posterior termina limpia.
    REQUIRE(rig.stopAndJoin() <= 1);
    requireNoShutdownEvents(*rig.receiver);
    rig.drain();
    REQUIRE(rig.pool.inUse() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// pollTimeoutMs: la parada acotada depende de la espera acotada
// ─────────────────────────────────────────────────────────────────────────────

namespace {

pw::Result<Ipv4Endpoint, pw::net::SocketError> openWithPollTimeout(const std::uint32_t ms,
                                                                   Ipv4Endpoint& localAfter)
{
    pw::mem::BufferPool      pool{8, 512};
    auto                     raw = std::make_unique<Queue>();
    auto                     rec = std::make_unique<Queue>();
    pw::obs::MetricsRegistry metrics;
    pw::obs::Logger          logger{pw::obs::LoggerConfig{}};

    pw::net::UdpReceiverConfig config;
    config.bindAddress   = "127.0.0.1";
    config.port          = 0;
    config.pollTimeoutMs = ms;

    Receiver receiver{config,  pool,   *raw, *rec, metrics, logger, pw::net::makeUdpSocket(),
                      &pw::monotonicNowNs, &pw::test::fakePriorityRaised};
    auto opened = receiver.open();
    localAfter  = receiver.localEndpoint();
    return opened;
}

} // namespace

TEST_CASE("pollTimeoutMs = 0 se rechaza antes de tocar el socket",
          "[net][receiver][shutdown][config][F1.1-12]")
{
    Ipv4Endpoint local;
    const auto   opened = openWithPollTimeout(0, local);

    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().kind == SocketErrorKind::InvalidConfiguration);
    REQUIRE(opened.error().describe().find("pollTimeoutMs") != std::string::npos);
    REQUIRE(local.port() == 0);   // no llego a enlazar

    // El tipo nuevo tiene nombre y consejo propios: no cae en «unknown».
    const std::string name{pw::net::socketErrorName(SocketErrorKind::InvalidConfiguration)};
    const std::string advice{pw::net::socketErrorAdvice(SocketErrorKind::InvalidConfiguration)};
    REQUIRE(name == "invalid_configuration");
    REQUIRE(name != pw::net::socketErrorName(SocketErrorKind::Unknown));
    REQUIRE(advice != pw::net::socketErrorAdvice(SocketErrorKind::Unknown));
}

TEST_CASE("pollTimeoutMs de 1 ms y el valor por defecto se aceptan",
          "[net][receiver][shutdown][config][F1.1-12]")
{
    STATIC_REQUIRE(pw::net::UdpReceiverConfig{}.pollTimeoutMs == 100);

    for (const std::uint32_t ms : {1U, 10U, 100U}) {
        INFO("pollTimeoutMs = " << ms);
        Ipv4Endpoint local;
        const auto   opened = openWithPollTimeout(ms, local);
        REQUIRE(opened.has_value());
        REQUIRE(local.port() != 0);
    }
}
