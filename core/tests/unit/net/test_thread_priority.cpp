// Pruebas de la prioridad de T1 (F1.1-11).
//
// La logica se prueba con un raiser falso inyectado: cuenta las llamadas, anota
// desde que hilo llego cada una y devuelve el resultado que la prueba elija. Asi
// ninguna prueba depende de que el sistema conceda de verdad una prioridad.
//
// La unica prueba contra el sistema real es tolerante: si Windows concede el
// cambio, se comprueba que es exactamente ABOVE_NORMAL y que el proceso no ha
// cambiado de clase; si lo rechaza, se avisa y no se falla.
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

#include "pw/mem/buffer_pool.hpp"
#include "pw/mem/spsc_ring.hpp"
#include "pw/net/thread_priority.hpp"
#include "pw/net/udp_receiver.hpp"
#include "pw/obs/logging.hpp"
#include "pw/obs/metrics.hpp"
#include "scripted_socket.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

using namespace std::chrono_literals;

using pw::net::Ipv4Endpoint;
using pw::net::ReceivedDatagram;
using pw::net::ThreadPriorityResult;
using pw::net::UdpReceiverState;
using pw::test::ScriptedDatagram;
using pw::test::scriptedError;
using pw::test::ScriptedSocket;

namespace {

// ── Raiser falso ────────────────────────────────────────────────────────────

std::atomic<int>             fakeCalls{0};
std::mutex                   fakeThreadsMutex;
std::vector<std::thread::id> fakeThreads;

void recordCall() noexcept
{
    fakeCalls.fetch_add(1, std::memory_order_relaxed);
    const std::lock_guard lock{fakeThreadsMutex};
    // La prueba reserva antes: aqui no hay reasignacion.
    if (fakeThreads.size() < fakeThreads.capacity()) {
        fakeThreads.push_back(std::this_thread::get_id());
    }
}

ThreadPriorityResult fakeRaised() noexcept
{
    recordCall();
    return ThreadPriorityResult::Raised;
}

ThreadPriorityResult fakeFailed() noexcept
{
    recordCall();
    return ThreadPriorityResult::Failed;
}

void resetFake()
{
    fakeCalls.store(0, std::memory_order_relaxed);
    const std::lock_guard lock{fakeThreadsMutex};
    fakeThreads.clear();
    fakeThreads.reserve(16);
}

std::vector<std::thread::id> fakeCallThreads()
{
    const std::lock_guard lock{fakeThreadsMutex};
    return fakeThreads;
}

// ── Reloj manual, para comprobar que la maquina de estados no cambia ────────

constexpr std::uint64_t    kMs = 1'000'000ULL;
std::atomic<std::uint64_t> manualNowNs{1'000 * kMs};

std::uint64_t manualClock() noexcept { return manualNowNs.load(std::memory_order_acquire); }

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

std::vector<ScriptedDatagram> datagrams(const std::size_t n)
{
    return std::vector<ScriptedDatagram>(n, ScriptedDatagram{kGame, 64});
}

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

/// Receptor con socket de guion y raiser elegido. No arranca ningun hilo: cada
/// prueba decide donde corre run().
struct PriorityRig {
    PriorityRig(const std::vector<ScriptedDatagram>& script, const pw::net::ThreadPriorityRaiser raiser)
        : pool{64, 512}
        , raw{std::make_unique<Queue>()}
        , rec{std::make_unique<Queue>()}
        , logger{loggerConfig()}
        , sink{std::make_shared<MemorySink>()}
    {
        logger.addSink(sink);
        auto scripted = std::make_unique<ScriptedSocket>(script);
        socket        = scripted.get();
        receiver      = std::make_unique<Receiver>(pw::net::UdpReceiverConfig{}, pool, *raw, *rec,
                                              metrics, logger, std::move(scripted), &manualClock,
                                              raiser);
        REQUIRE(receiver->open().has_value());
    }

    ~PriorityRig() { drain(); }

    void drain()
    {
        ReceivedDatagram item;
        while (raw->tryPop(item)) { pool.release(item.handle); }
        while (rec->tryPop(item)) { pool.release(item.handle); }
    }

    /// Ejecuta run() en un hilo propio hasta que se hayan entregado `target`
    /// entradas del guion (todas, por defecto) y T1 haya dado dos vueltas mas, y
    /// devuelve el id de ese hilo.
    std::thread::id runUntilScriptDelivered(std::size_t target = 0)
    {
        if (target == 0) { target = socket->total(); }
        std::stop_source stopper;
        std::thread      worker{[&] { receiver->run(stopper.get_token()); }};
        const auto       id = worker.get_id();
        REQUIRE(waitUntil([&] {
            drain();
            return socket->delivered() == target;
        }));
        const auto from = socket->idlePolls();
        REQUIRE(waitUntil([&] { return socket->idlePolls() >= from + 2; }));
        stopper.request_stop();
        worker.join();
        drain();
        return id;
    }

    static pw::obs::LoggerConfig loggerConfig()
    {
        pw::obs::LoggerConfig config;
        config.level               = pw::obs::LogLevel::Info;
        config.aggregationWindowMs = 0;
        return config;
    }

    pw::mem::BufferPool         pool;
    std::unique_ptr<Queue>      raw;
    std::unique_ptr<Queue>      rec;
    pw::obs::MetricsRegistry    metrics;
    pw::obs::Logger             logger;
    std::shared_ptr<MemorySink> sink;
    ScriptedSocket*             socket{nullptr};
    std::unique_ptr<Receiver>   receiver;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Logica, con raiser falso
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Antes de run() la prioridad no se ha intentado", "[net][receiver][priority][F1.1-11]")
{
    resetFake();
    PriorityRig rig{datagrams(1), &fakeRaised};

    REQUIRE(rig.receiver->threadPriority() == ThreadPriorityResult::NotAttempted);
    REQUIRE(fakeCalls.load() == 0);   // construir y abrir no lo intentan
}

TEST_CASE("Con Raised: una sola llamada, desde el hilo de run(), y el receptor igual",
          "[net][receiver][priority][F1.1-11]")
{
    resetFake();
    constexpr std::size_t kDatagrams = 40;
    PriorityRig           rig{datagrams(kDatagrams), &fakeRaised};

    const auto t1 = rig.runUntilScriptDelivered();

    // Una llamada, aunque hayan pasado 40 datagramas y varias esperas vencidas.
    REQUIRE(fakeCalls.load() == 1);
    const auto threads = fakeCallThreads();
    REQUIRE(threads.size() == 1);
    REQUIRE(threads[0] == t1);                              // desde T1
    REQUIRE(threads[0] != std::this_thread::get_id());      // y no desde quien lo arranco

    REQUIRE(rig.receiver->threadPriority() == ThreadPriorityResult::Raised);
    REQUIRE(rig.receiver->stats().received == kDatagrams);
    REQUIRE(rig.receiver->stats().delivered == kDatagrams);
    REQUIRE(rig.receiver->state() == UdpReceiverState::Receiving);
    REQUIRE_FALSE(rig.receiver->eventSeen(pw::net::ReceiverEvent::PriorityNotRaised));
    REQUIRE(rig.receiver->reportPendingEvents() == 0);
}

TEST_CASE("Con Failed: el receptor sigue recibiendo, sin reintentos, y avisa fuera de T1",
          "[net][receiver][priority][F1.1-11]")
{
    resetFake();
    constexpr std::size_t kDatagrams = 40;
    PriorityRig           rig{datagrams(kDatagrams), &fakeFailed};

    rig.runUntilScriptDelivered();

    REQUIRE(fakeCalls.load() == 1);   // ni un reintento por datagrama
    REQUIRE(rig.receiver->threadPriority() == ThreadPriorityResult::Failed);
    REQUIRE(rig.receiver->stats().received == kDatagrams);
    REQUIRE(rig.receiver->stats().delivered == kDatagrams);
    REQUIRE(rig.receiver->eventSeen(pw::net::ReceiverEvent::PriorityNotRaised));

    // T1 no ha escrito nada; el aviso lo emite quien esta fuera, una vez.
    rig.logger.flush();
    REQUIRE(rig.sink->countContaining("prioridad del hilo de recepcion") == 0);
    REQUIRE(rig.receiver->reportPendingEvents() == 1);
    REQUIRE(rig.receiver->reportPendingEvents() == 0);
    rig.logger.flush();
    REQUIRE(rig.sink->countContaining("prioridad del hilo de recepcion") == 1);
}

TEST_CASE("Con Failed, Listening, Receiving y Silent funcionan exactamente igual",
          "[net][receiver][priority][state][F1.1-11]")
{
    resetFake();
    manualNowNs.store(1'000 * kMs);
    PriorityRig rig{datagrams(1), &fakeFailed};
    rig.socket->allowUpTo(0);

    std::stop_source stopper;
    std::thread      worker{[&] { rig.receiver->run(stopper.get_token()); }};
    const auto       settle = [&] {
        const auto from = rig.socket->idlePolls();
        REQUIRE(waitUntil([&] { return rig.socket->idlePolls() >= from + 2; }));
    };

    settle();
    REQUIRE(rig.receiver->state() == UdpReceiverState::Listening);

    rig.socket->allowUpTo(1);
    REQUIRE(waitUntil([&] { return rig.socket->delivered() == 1; }));
    settle();
    REQUIRE(rig.receiver->state() == UdpReceiverState::Receiving);

    manualNowNs.store(3'000 * kMs);   // 2000 ms: vence el silencio
    settle();
    REQUIRE(rig.receiver->state() == UdpReceiverState::Silent);
    REQUIRE(rig.receiver->silenceTransitions() == 1);

    stopper.request_stop();
    worker.join();
    REQUIRE(fakeCalls.load() == 1);
}

TEST_CASE("El cierre del socket y la parada no cambian con la prioridad",
          "[net][receiver][priority][F1.1-11]")
{
    resetFake();
    const std::vector<ScriptedDatagram> script{ScriptedDatagram{kGame, 64},
                                               scriptedError(pw::net::SocketErrorKind::Closed)};
    PriorityRig rig{script, &fakeRaised};

    std::stop_source never;
    rig.receiver->run(never.get_token());   // vuelve por el cierre

    REQUIRE(rig.receiver->eventSeen(pw::net::ReceiverEvent::SocketClosed));
    REQUIRE(rig.receiver->threadPriority() == ThreadPriorityResult::Raised);
    REQUIRE(fakeCalls.load() == 1);
}

TEST_CASE("Cada llamada a run() intenta la prioridad de su propio hilo, una vez",
          "[net][receiver][priority][F1.1-11]")
{
    // La prioridad es del hilo, y run() puede ejecutarse en hilos distintos (las
    // pruebas lo hacen). En produccion run() se llama una vez.
    resetFake();
    PriorityRig rig{datagrams(4), &fakeRaised};

    rig.socket->allowUpTo(2);
    const auto first = rig.runUntilScriptDelivered(2);   // se detiene al llegar al tope
    rig.socket->allowUpTo(4);
    const auto second = rig.runUntilScriptDelivered(4);

    REQUIRE(fakeCalls.load() == 2);
    const auto threads = fakeCallThreads();
    REQUIRE(threads.size() == 2);
    REQUIRE(threads[0] == first);
    REQUIRE(threads[1] == second);
}

// ─────────────────────────────────────────────────────────────────────────────
// Adaptadores reales
// ─────────────────────────────────────────────────────────────────────────────

#if defined(_WIN32)

TEST_CASE("Windows: ABOVE_NORMAL solo en el hilo que llama, y el proceso no cambia de clase",
          "[net][priority][windows][F1.1-11]")
{
    const auto processClassBefore = GetPriorityClass(GetCurrentProcess());
    const auto callerBefore       = GetThreadPriority(GetCurrentThread());

    ThreadPriorityResult result          = ThreadPriorityResult::NotAttempted;
    int                  workerPriority  = THREAD_PRIORITY_ERROR_RETURN;
    std::thread          worker{[&] {
        result         = pw::net::raiseCurrentThreadAboveNormal();
        workerPriority = GetThreadPriority(GetCurrentThread());
    }};
    worker.join();

    // El proceso no cambia de clase, y el hilo que NO llamo tampoco cambia.
    REQUIRE(GetPriorityClass(GetCurrentProcess()) == processClassBefore);
    REQUIRE(GetThreadPriority(GetCurrentThread()) == callerBefore);

    REQUIRE((result == ThreadPriorityResult::Raised || result == ThreadPriorityResult::Failed));
    if (result == ThreadPriorityResult::Raised) {
        // Exactamente ABOVE_NORMAL: ni HIGHEST ni TIME_CRITICAL.
        REQUIRE(workerPriority == THREAD_PRIORITY_ABOVE_NORMAL);
    } else {
        WARN("el sistema ha rechazado THREAD_PRIORITY_ABOVE_NORMAL en esta maquina; "
             "la logica de degradacion ya se prueba con el raiser falso");
    }
}

TEST_CASE("Windows: el receptor real solo cambia la prioridad de T1",
          "[net][receiver][priority][windows][F1.1-11]")
{
    const auto processClassBefore = GetPriorityClass(GetCurrentProcess());
    const auto callerBefore       = GetThreadPriority(GetCurrentThread());

    PriorityRig rig{datagrams(3), &pw::net::raiseCurrentThreadAboveNormal};
    rig.runUntilScriptDelivered();

    const auto result = rig.receiver->threadPriority();
    REQUIRE((result == ThreadPriorityResult::Raised || result == ThreadPriorityResult::Failed));
    REQUIRE(rig.receiver->stats().received == 3);
    REQUIRE(GetPriorityClass(GetCurrentProcess()) == processClassBefore);
    REQUIRE(GetThreadPriority(GetCurrentThread()) == callerBefore);
}

#else

TEST_CASE("POSIX: no-op deliberado, devuelve Unsupported", "[net][priority][posix][F1.1-11]")
{
    REQUIRE(pw::net::raiseCurrentThreadAboveNormal() == ThreadPriorityResult::Unsupported);

    // Y Unsupported no es un fallo: el receptor no avisa.
    PriorityRig rig{datagrams(1), &pw::net::raiseCurrentThreadAboveNormal};
    rig.runUntilScriptDelivered();
    REQUIRE(rig.receiver->threadPriority() == ThreadPriorityResult::Unsupported);
    REQUIRE_FALSE(rig.receiver->eventSeen(pw::net::ReceiverEvent::PriorityNotRaised));
}

#endif
