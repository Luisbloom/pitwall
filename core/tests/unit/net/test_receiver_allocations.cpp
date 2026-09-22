// ─────────────────────────────────────────────────────────────────────────────
// COMPILACION INSTRUMENTADA: cero asignaciones por datagrama (F1.1-07, RNF-013).
//
// La DoD de F1.1 no pide que el codigo «este disenado para no asignar»: pide
// «0 asignaciones por datagrama, verificado con compilacion instrumentada». Este
// ejecutable es esa compilacion: sustituye los operadores globales de reserva de
// memoria y cuenta las que ocurren EN EL HILO DEL RECEPTOR.
//
// Vive en su propio binario porque la sustitucion de operator new es global: asi
// no afecta a ninguna otra prueba y se ve de un vistazo que esta instrumentado.
//
// ── QUE SE MIDE Y QUE NO ────────────────────────────────────────────────────
//
// Se cuenta solo en el hilo del receptor, y solo despues del calentamiento. Lo
// que queda fuera es deliberado:
//
//   - el emisor y el armazon de la prueba viven en otro hilo y no se cuentan;
//   - el arranque -construir el pool, enlazar el socket, resolver contadores-
//     asigna, y debe asignar: preasignar es justamente lo que permite que el
//     bucle no asigne;
//   - el calentamiento absorbe lo que se reserva la primera vez que se recorre
//     un camino: buffers internos del sistema, estados perezosos, avisos.
//
// ── DOS MEDICIONES, Y POR QUE HACEN FALTA LAS DOS ───────────────────────────
//
//   COLD PATH      el contador se arma ANTES del primer datagrama, sin ningun
//                  calentamiento. Demuestra que ni siquiera el PRIMERO de cada
//                  camino -incluida la primera vez que se detecta una causa
//                  excepcional- provoca una asignacion en T1.
//
//   STEADY STATE   10.000 validos y 2.000 truncados tras un calentamiento.
//                  Demuestra que el coste por datagrama sigue siendo cero
//                  cuando el sistema lleva rato funcionando.
//
// La primera es la que obligo a cambiar el codigo. La version anterior del
// receptor llamaba a warnOnce() -que formatea y por tanto asigna- la primera
// vez que veia cada causa: seis asignaciones como mucho en toda la vida del
// receptor, pero seis datagramas concretos las habrian provocado. «Cero por
// datagrama» (RNF-013) incluye al primero, asi que T1 dejo de registrar nada:
// ahora marca una bandera preasignada y el texto lo emite reportPendingEvents()
// desde fuera del camino caliente.
// ─────────────────────────────────────────────────────────────────────────────
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <array>
#include <cstdlib>
#include <string>
#include <memory>
#include <new>
#include <stop_token>
#include <thread>
#include <vector>

#include "pw/mem/buffer_pool.hpp"
#include "pw/mem/spsc_ring.hpp"
#include "pw/net/udp_receiver.hpp"
#include "pw/obs/logging.hpp"
#include "pw/obs/metrics.hpp"
#include "scripted_socket.hpp"

using namespace std::chrono_literals;

using pw::net::Ipv4Endpoint;
using pw::net::kLoopbackAddress;
using pw::net::ReceivedDatagram;

// ── Instrumentacion ─────────────────────────────────────────────────────────

namespace alloc_probe {

/// Se activa DENTRO del hilo que se quiere medir. Al ser thread_local, ningun
/// otro hilo cuenta, y contar no cuesta ni una sincronizacion.
thread_local bool counting = false;

std::atomic<std::uint64_t> allocations{0};

/// Tamanos de las primeras asignaciones observadas. Cuando la cuenta no es
/// cero, el tamano es la pista mas barata para identificar de donde sale.
std::array<std::size_t, 32> observedSizes{};
std::atomic<std::size_t>    observedCount{0};


void reset() noexcept
{
    allocations.store(0, std::memory_order_relaxed);
    observedCount.store(0, std::memory_order_relaxed);
    observedSizes.fill(0);
}

[[nodiscard]] std::string describeSizes()
{
    const auto total = observedCount.load(std::memory_order_relaxed);
    std::string out;
    for (std::size_t i = 0; i < total && i < observedSizes.size(); ++i) {
        out += std::to_string(observedSizes[i]);
        out += " B; ";
    }
    return out;
}
[[nodiscard]] std::uint64_t count() noexcept
{
    return allocations.load(std::memory_order_relaxed);
}

inline void note(const std::size_t size) noexcept
{
    if (!counting) { return; }
    allocations.fetch_add(1, std::memory_order_relaxed);
    const auto slot = observedCount.fetch_add(1, std::memory_order_relaxed);
    if (slot < observedSizes.size()) { observedSizes[slot] = size; }
}

[[nodiscard]] void* allocate(const std::size_t size)
{
    note(size);
    // Un tamano de 0 debe devolver un puntero unico, no nulo.
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) { throw std::bad_alloc{}; }
    return memory;
}

[[nodiscard]] void* allocateAligned(const std::size_t size, const std::align_val_t alignment)
{
    note(size);
    const auto bytes = size == 0 ? 1 : size;
#if defined(_MSC_VER)
    void* memory = _aligned_malloc(bytes, static_cast<std::size_t>(alignment));
#else
    const auto boundary = static_cast<std::size_t>(alignment);
    const auto rounded  = ((bytes + boundary - 1) / boundary) * boundary;
    void*      memory   = std::aligned_alloc(boundary, rounded);
#endif
    if (memory == nullptr) { throw std::bad_alloc{}; }
    return memory;
}

void releaseAligned(void* memory) noexcept
{
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

} // namespace alloc_probe

// Sustitucion de los operadores globales. Estan todos a proposito: emparejar mal
// una version alineada con una normal seria un fallo mucho peor que el que se
// intenta medir.
void* operator new(std::size_t size) { return alloc_probe::allocate(size); }
void* operator new[](std::size_t size) { return alloc_probe::allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment)
{
    return alloc_probe::allocateAligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return alloc_probe::allocateAligned(size, alignment);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept
{
    alloc_probe::releaseAligned(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept
{
    alloc_probe::releaseAligned(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
    alloc_probe::releaseAligned(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
    alloc_probe::releaseAligned(memory);
}

// ── La prueba ───────────────────────────────────────────────────────────────

namespace {

/// Colas de produccion: 8192 huecos cada una (04-ARCHITECTURE.md 4.3).
using RawQueue = pw::net::RawDatagramQueue;
using RecQueue = pw::net::RecDatagramQueue;

/// Colas diminutas, para poder llenarlas con cuatro datagramas.
using TinyRawQueue = pw::mem::SpscRing<ReceivedDatagram, 4>;
using TinyRecQueue = pw::mem::SpscRing<ReceivedDatagram, 4>;
using TinyReceiver = pw::net::UdpReceiverT<TinyRawQueue, TinyRecQueue>;

/// Socket que devuelve siempre un fallo elegido.
///
/// Es la unica forma razonable de recorrer el camino de «error de socket que no
/// es espera vencida»: un fallo asi no se puede provocar desde fuera sin romper
/// algo del sistema. El receptor acepta el socket por constructor precisamente
/// para esto, asi que no es infraestructura inventada para la prueba: es la
/// costura que ya existia.
///
/// No asigna en receive(): si asignara, mediria la prueba en vez del receptor.
class FailingSocket final : public pw::net::IUdpSocket {
public:
    explicit FailingSocket(const pw::net::SocketErrorKind kind) noexcept : kind_{kind} {}

    pw::Result<void, pw::net::SocketError> bind(const Ipv4Endpoint&) override { return {}; }
    pw::Result<std::size_t, pw::net::SocketError> setReceiveBufferBytes(std::size_t n) override
    {
        return n;
    }
    pw::Result<std::size_t, pw::net::SocketError> receiveBufferBytes() const override
    {
        return std::size_t{0};
    }
    pw::Result<void, pw::net::SocketError> setReceivePollTimeout(std::uint32_t) override
    {
        return {};
    }
    pw::Result<pw::net::ReceivedBytes, pw::net::ReceiveError> receive(std::span<std::byte>) override
    {
        failures_.fetch_add(1, std::memory_order_relaxed);
        return pw::Unexpected<pw::net::ReceiveError>{pw::net::ReceiveError{kind_, 0}};
    }
    Ipv4Endpoint localEndpoint() const noexcept override { return {}; }
    pw::Result<std::size_t, pw::net::SocketError> sendTo(const Ipv4Endpoint&,
                                                          std::span<const std::byte>) override
    {
        return std::size_t{0};
    }
    void close() noexcept override {}
    bool isOpen() const noexcept override { return true; }

    [[nodiscard]] std::uint64_t failures() const noexcept
    {
        return failures_.load(std::memory_order_relaxed);
    }

private:
    pw::net::SocketErrorKind   kind_;
    std::atomic<std::uint64_t> failures_{0};
};

template <typename Predicate>
bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout = 30s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) { return true; }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

struct Measurement {
    std::uint64_t allocations{0};
    std::uint64_t datagrams{0};
    std::string   sizes{};
};

/// Ejecuta el receptor en un hilo propio, con calentamiento y medicion, y
/// devuelve cuantas asignaciones ocurrieron EN ESE HILO durante la medicion.
///
/// `payloadBytes` decide que camino del bucle se recorre: uno valido se reparte
/// a las dos colas; uno de 10 bytes cae por la longitud minima.
Measurement measure(const std::size_t payloadBytes, const int measuredDatagrams)
{
    pw::mem::BufferPool pool{pw::mem::kMinimumPoolSize, 2048};
    auto                rawQueue = std::make_unique<RawQueue>();
    auto                recQueue = std::make_unique<RecQueue>();

    pw::obs::MetricsRegistry metrics;
    pw::obs::LoggerConfig    loggerConfig;
    loggerConfig.aggregationWindowMs = 60'000;
    pw::obs::Logger logger{loggerConfig};

    pw::net::UdpReceiverConfig config;
    config.bindAddress   = "127.0.0.1";
    config.port          = 0;
    config.pollTimeoutMs = 5;

    pw::net::UdpReceiver receiver{config, pool, *rawQueue, *recQueue, metrics, logger};
    const auto           opened = receiver.open();
    REQUIRE(opened.has_value());

    auto sender = pw::net::makeUdpSocket();
    REQUIRE(sender->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());
    const std::vector<std::byte> payload(payloadBytes, std::byte{0x5C});

    const auto drain = [&] {
        ReceivedDatagram datagram;
        while (rawQueue->tryPop(datagram)) { pool.release(datagram.handle); }
        while (recQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    };

    std::stop_source  warmUpStop;
    std::stop_source  measuredStop;
    std::atomic<bool> warmedUp{false};

    std::thread worker{[&] {
        // ── Calentamiento ───────────────────────────────────────────────────
        receiver.run(warmUpStop.get_token());
        warmedUp.store(true, std::memory_order_release);

        // ── Medicion: a partir de aqui se cuenta TODO lo que asigne este hilo
        alloc_probe::counting = true;
        receiver.run(measuredStop.get_token());
        alloc_probe::counting = false;
    }};

    // Calentamiento: unos cuantos datagramas por el mismo camino que se va a
    // medir, para que cualquier reserva perezosa -incluido el aviso unico de
    // warnOnce si este camino lo dispara- ocurra ANTES de contar.
    constexpr int kWarmUpDatagrams = 200;
    for (int i = 0; i < kWarmUpDatagrams; ++i) {
        REQUIRE(sender->sendTo(opened.value(), payload).has_value());
        if ((i % 50) == 0) { drain(); }
    }
    REQUIRE(waitUntil([&] {
        drain();
        return receiver.stats().received >= kWarmUpDatagrams;
    }));

    warmUpStop.request_stop();
    REQUIRE(waitUntil([&] { return warmedUp.load(std::memory_order_acquire); }));

    alloc_probe::reset();
    const auto before = receiver.stats().received;

    // ── Medicion ────────────────────────────────────────────────────────────
    for (int i = 0; i < measuredDatagrams; ++i) {
        REQUIRE(sender->sendTo(opened.value(), payload).has_value());
        if ((i % 100) == 0) { drain(); }
    }
    REQUIRE(waitUntil([&] {
        drain();
        return receiver.stats().received >= before + measuredDatagrams;
    }));

    measuredStop.request_stop();
    worker.join();
    drain();

    const auto measured = Measurement{alloc_probe::count(),
                                      receiver.stats().received - before,
                                      alloc_probe::describeSizes()};

    REQUIRE(pool.inUse() == 0);
    return measured;
}

} // namespace

TEST_CASE("STEADY STATE: cero asignaciones en el camino normal",
          "[net][receiver][allocations][steady][RNF-013][F1.1-07]")
{
    constexpr int kDatagrams = 10'000;

    const auto measured = measure(1470, kDatagrams);

    WARN("camino normal: " << measured.datagrams << " datagramas de 1470 B procesados con "
                           << measured.allocations << " asignaciones en el hilo del receptor  ["
                           << measured.sizes << "]");

    REQUIRE(measured.datagrams >= kDatagrams);
    REQUIRE(measured.allocations == 0);
}

TEST_CASE("STEADY STATE: cero asignaciones en el camino de los truncados",
          "[net][receiver][allocations][steady][RNF-013][RF-004]")
{
    // El camino de error, con su causa ya calentada: si el aviso de warnOnce no
    // estuviera acotado a una vez, dos mil truncados lo destaparian.
    constexpr int kDatagrams = 2'000;

    const auto measured = measure(10, kDatagrams);

    WARN("camino truncado: " << measured.datagrams << " datagramas de 10 B procesados con "
                             << measured.allocations << " asignaciones en el hilo del receptor  ["
                             << measured.sizes << "]");

    REQUIRE(measured.datagrams >= kDatagrams);
    REQUIRE(measured.allocations == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// COLD PATH: el contador se arma antes del primer datagrama
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Cero asignaciones desde el PRIMER datagrama, con las colas llenandose",
          "[net][receiver][allocations][coldpath][RNF-013][F1.1-07]")
{
    // Casos A, B, C, D y E. Todo el montaje esta hecho antes de armar el
    // contador: pool construido, colas construidas, metricas resueltas, socket
    // abierto. Lo que se mide es solo lo que hace T1 desde su primera vuelta.
    //
    //   A  primer datagrama valido
    //   B  primer truncado
    //   C  primer oversized
    //   D  primera Qraw llena
    //   E  primera Qrec llena
    //
    // El pool se dimensiona con la regla del modelo de propiedad -la suma de las
    // dos colas mas los buffers en vuelo- para que las colas lleguen a llenarse.
    // Con un pool menor, el pool se agotaria ANTES y las colas no se llenarian
    // nunca: es exactamente el fenomeno que midio F1.1-01, y aqui se nota igual.
    constexpr std::size_t kQueueCapacity = 4;

    pw::mem::BufferPool pool{pw::mem::poolSizeForQueues(kQueueCapacity * 2, 2), 512};
    auto                rawQueue = std::make_unique<TinyRawQueue>();
    auto                recQueue = std::make_unique<TinyRecQueue>();

    pw::obs::MetricsRegistry metrics;
    pw::obs::LoggerConfig    loggerConfig;
    loggerConfig.aggregationWindowMs = 60'000;
    pw::obs::Logger logger{loggerConfig};

    pw::net::UdpReceiverConfig config;
    config.bindAddress            = "127.0.0.1";
    config.port                   = 0;
    config.pollTimeoutMs          = 5;
    config.poolExhaustedBackoffUs = 200;

    TinyReceiver receiver{config, pool, *rawQueue, *recQueue, metrics, logger};
    const auto   opened = receiver.open();
    REQUIRE(opened.has_value());

    auto sender = pw::net::makeUdpSocket();
    REQUIRE(sender->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());

    alloc_probe::reset();

    std::stop_source stopper;
    std::thread      worker{[&] {
        // Armado ANTES de que exista ningun datagrama. Sin calentamiento.
        alloc_probe::counting = true;
        receiver.run(stopper.get_token());
        alloc_probe::counting = false;
    }};

    const auto send = [&](const std::size_t bytes) {
        const std::vector<std::byte> payload(bytes, std::byte{0x11});
        REQUIRE(sender->sendTo(opened.value(), payload).has_value());
    };

    send(64);    // A: valido
    send(10);    // B: truncado
    send(600);   // C: mayor que el buffer del pool, que es de 512 B
    for (int i = 0; i < 12; ++i) { send(64); }   // D y E: nadie drena

    const auto reachedAll = waitUntil([&] {
        return receiver.eventSeen(pw::net::ReceiverEvent::Truncated)
               && receiver.eventSeen(pw::net::ReceiverEvent::Oversized)
               && receiver.eventSeen(pw::net::ReceiverEvent::QueueRawFull)
               && receiver.eventSeen(pw::net::ReceiverEvent::QueueRecFull);
    });

    stopper.request_stop();
    worker.join();

    const auto allocations = alloc_probe::count();
    const auto stats       = receiver.stats();

    WARN("COLD PATH colas: " << stats.received << " recibidos, " << stats.truncated
                             << " truncados, " << stats.oversized << " oversized, Qraw llena "
                             << stats.qrawDropped << ", Qrec llena " << stats.qrecDropped
                             << "  ->  " << allocations << " asignaciones en T1  ["
                             << alloc_probe::describeSizes() << "]");

    REQUIRE(reachedAll);
    REQUIRE(stats.received >= 1);
    REQUIRE(stats.truncated >= 1);
    REQUIRE(stats.oversized >= 1);
    REQUIRE(stats.qrawDropped >= 1);
    REQUIRE(stats.qrecDropped >= 1);

    REQUIRE(allocations == 0);

    ReceivedDatagram datagram;
    while (rawQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    while (recQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    REQUIRE(pool.inUse() == 0);
}

TEST_CASE("Cero asignaciones en el primer agotamiento del pool",
          "[net][receiver][allocations][coldpath][RNF-013]")
{
    // Caso F, aparte, porque no cabe con el anterior: para que el pool se agote
    // tiene que ser MENOR que lo que las colas retienen, y para que las colas se
    // llenen tiene que ser mayor. Las dos condiciones se excluyen, asi que se
    // miden por separado en vez de forzar un montaje que no representa nada.
    pw::mem::BufferPool pool{2, 512};
    auto                rawQueue = std::make_unique<TinyRawQueue>();
    auto                recQueue = std::make_unique<TinyRecQueue>();

    pw::obs::MetricsRegistry metrics;
    pw::obs::Logger          logger{pw::obs::LoggerConfig{}};

    pw::net::UdpReceiverConfig config;
    config.bindAddress            = "127.0.0.1";
    config.port                   = 0;
    config.pollTimeoutMs          = 5;
    config.poolExhaustedBackoffUs = 200;

    TinyReceiver receiver{config, pool, *rawQueue, *recQueue, metrics, logger};
    const auto   opened = receiver.open();
    REQUIRE(opened.has_value());

    auto sender = pw::net::makeUdpSocket();
    REQUIRE(sender->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());

    alloc_probe::reset();

    std::stop_source stopper;
    std::thread      worker{[&] {
        alloc_probe::counting = true;
        receiver.run(stopper.get_token());
        alloc_probe::counting = false;
    }};

    const std::vector<std::byte> payload(64, std::byte{0x22});
    for (int i = 0; i < 8; ++i) {
        REQUIRE(sender->sendTo(opened.value(), payload).has_value());
    }

    const auto exhausted =
        waitUntil([&] { return receiver.eventSeen(pw::net::ReceiverEvent::PoolExhausted); });

    stopper.request_stop();
    worker.join();

    const auto allocations = alloc_probe::count();

    WARN("COLD PATH pool agotado: " << receiver.stats().poolExhaustions
                                    << " agotamientos  ->  " << allocations
                                    << " asignaciones en T1  [" << alloc_probe::describeSizes()
                                    << "]");

    REQUIRE(exhausted);
    REQUIRE(receiver.stats().poolExhaustions >= 1);
    REQUIRE(allocations == 0);

    ReceivedDatagram datagram;
    while (rawQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    while (recQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    REQUIRE(pool.inUse() == 0);
}

TEST_CASE("Cero asignaciones en el primer error de socket",
          "[net][receiver][allocations][coldpath][RNF-013]")
{
    // Caso G. Un fallo de socket que no sea espera vencida no se puede provocar
    // desde fuera con un socket real sin romper algo del sistema, asi que se
    // inyecta por la costura que el receptor ya tenia: el socket se le pasa por
    // constructor.
    pw::mem::BufferPool pool{4, 512};
    auto                rawQueue = std::make_unique<TinyRawQueue>();
    auto                recQueue = std::make_unique<TinyRecQueue>();

    pw::obs::MetricsRegistry metrics;
    pw::obs::LoggerConfig    loggerConfig;
    loggerConfig.aggregationWindowMs = 60'000;
    pw::obs::Logger logger{loggerConfig};

    pw::net::UdpReceiverConfig config;
    config.poolExhaustedBackoffUs = 100;

    auto        failing = std::make_unique<FailingSocket>(pw::net::SocketErrorKind::Unknown);
    auto* const socket  = failing.get();

    TinyReceiver receiver{config, pool, *rawQueue, *recQueue, metrics, logger, std::move(failing)};
    REQUIRE(receiver.open().has_value());

    alloc_probe::reset();

    std::stop_source stopper;
    std::thread      worker{[&] {
        alloc_probe::counting = true;
        receiver.run(stopper.get_token());
        alloc_probe::counting = false;
    }};

    const auto sawFailure =
        waitUntil([&] { return receiver.stats().socketErrors >= 50; });

    stopper.request_stop();
    worker.join();

    const auto allocations = alloc_probe::count();

    WARN("COLD PATH error de socket: " << receiver.stats().socketErrors << " fallos, "
                                       << socket->failures() << " llamadas a receive  ->  "
                                       << allocations << " asignaciones en T1  ["
                                       << alloc_probe::describeSizes() << "]");

    REQUIRE(sawFailure);
    REQUIRE(receiver.eventSeen(pw::net::ReceiverEvent::SocketFailure));
    REQUIRE(allocations == 0);
    REQUIRE(pool.inUse() == 0);   // ningun buffer se fuga por el camino de error
}

TEST_CASE("Cero asignaciones al cerrarse el socket bajo el bucle",
          "[net][receiver][allocations][coldpath]")
{
    // El septimo suceso: el bucle termina porque el socket esta cerrado. Antes
    // esto registraba un INFO desde T1; ahora solo marca la bandera.
    pw::mem::BufferPool pool{4, 512};
    auto                rawQueue = std::make_unique<TinyRawQueue>();
    auto                recQueue = std::make_unique<TinyRecQueue>();

    pw::obs::MetricsRegistry metrics;
    pw::obs::Logger          logger{pw::obs::LoggerConfig{}};

    auto failing = std::make_unique<FailingSocket>(pw::net::SocketErrorKind::Closed);

    TinyReceiver receiver{pw::net::UdpReceiverConfig{}, pool,   *rawQueue,
                          *recQueue,                   metrics, logger,
                          std::move(failing)};
    REQUIRE(receiver.open().has_value());

    alloc_probe::reset();

    std::stop_source  never;
    std::atomic<bool> returned{false};
    std::thread       worker{[&] {
        alloc_probe::counting = true;
        receiver.run(never.get_token());
        alloc_probe::counting = false;
        returned.store(true, std::memory_order_release);
    }};

    REQUIRE(waitUntil([&] { return returned.load(std::memory_order_acquire); }, 5000ms));
    worker.join();

    REQUIRE(receiver.eventSeen(pw::net::ReceiverEvent::SocketClosed));
    REQUIRE(alloc_probe::count() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tabla de origenes (F1.1-08, F1.1-09): cero asignaciones tambien al registrar
// ─────────────────────────────────────────────────────────────────────────────

namespace {

using pw::test::ScriptedDatagram;
using pw::test::ScriptedSocket;

constexpr Ipv4Endpoint lanHost(const std::size_t i)
{
    return Ipv4Endpoint{{192, 168, 1, static_cast<std::uint8_t>(10 + i)}, 20777};
}

std::uint64_t probeAllocations() noexcept { return alloc_probe::count(); }

} // namespace

TEST_CASE("COLD PATH: cero asignaciones al registrar origenes, uno a uno y con la tabla llena",
          "[net][receiver][allocations][coldpath][sources][RNF-013][F1.1-09]")
{
    // El contador se arma antes del primer datagrama. El socket de guion
    // consulta la sonda al empezar cada receive(), asi que la diferencia entre
    // dos llamadas seguidas es lo que asigno T1 procesando UN datagrama concreto.
    // Eso permite atribuir cada caso por separado en vez de dar solo un total.
    std::vector<ScriptedDatagram> script;
    script.push_back({lanHost(0), 1349});   // 0: primer datagrama del primer origen
    script.push_back({lanHost(1), 1349});   // 1: primer datagrama del segundo origen
    script.push_back({lanHost(0), 1349});   // 2: origen ya conocido
    for (std::size_t i = 2; i < pw::net::kMaxSources; ++i) {
        script.push_back({lanHost(i), 64});  // 3..8: se completa la tabla
    }
    const auto fullAt = script.size();
    script.push_back({lanHost(20), 64});    // primer origen con la tabla llena
    script.push_back({lanHost(0), 64});     // conocido, con la tabla llena
    script.push_back({lanHost(21), 64});    // otro origen sin sitio
    script.push_back({lanHost(22), 10});    // truncado de un origen sin sitio

    pw::mem::BufferPool pool{64, 2048};
    auto                rawQueue = std::make_unique<RawQueue>();
    auto                recQueue = std::make_unique<RecQueue>();

    pw::obs::MetricsRegistry metrics;
    pw::obs::Logger          logger{pw::obs::LoggerConfig{}};

    auto        scripted = std::make_unique<ScriptedSocket>(script, 1, &probeAllocations);
    auto* const socket   = scripted.get();

    pw::net::UdpReceiver receiver{pw::net::UdpReceiverConfig{}, pool, *rawQueue, *recQueue,
                                  metrics, logger, std::move(scripted)};
    REQUIRE(receiver.open().has_value());

    alloc_probe::reset();

    std::stop_source stopper;
    std::thread      worker{[&] {
        alloc_probe::counting = true;
        receiver.run(stopper.get_token());
        alloc_probe::counting = false;
    }};

    const auto all = waitUntil([&] { return socket->delivered() == socket->total(); });
    // Una vuelta mas del bucle para que la sonda capture el cierre del ultimo.
    std::this_thread::sleep_for(20ms);
    stopper.request_stop();
    worker.join();

    const auto during = [&](const std::size_t i) {
        return socket->probeAt(i + 1) - socket->probeAt(i);
    };

    const auto firstOfFirst  = during(0);
    const auto firstOfSecond = during(1);
    const auto known         = during(2);
    const auto firstFull     = during(fullAt);
    const auto knownFull     = during(fullAt + 1);
    const auto total         = alloc_probe::count();

    WARN("COLD PATH origenes: primer datagrama del primer origen "
         << firstOfFirst << ", del segundo " << firstOfSecond << ", conocido " << known
         << ", tabla llena " << firstFull << ", conocido con tabla llena " << knownFull
         << "  ->  total " << total << " asignaciones en T1  [" << alloc_probe::describeSizes()
         << "]");

    REQUIRE(all);
    REQUIRE(receiver.sources().size() == pw::net::kMaxSources);
    REQUIRE(receiver.sources().datagrams(pw::net::kUnregisteredSource) == 3);
    REQUIRE(receiver.eventSeen(pw::net::ReceiverEvent::SourceTableFull));

    REQUIRE(firstOfFirst == 0);
    REQUIRE(firstOfSecond == 0);
    REQUIRE(known == 0);
    REQUIRE(firstFull == 0);
    REQUIRE(knownFull == 0);
    REQUIRE(total == 0);

    ReceivedDatagram datagram;
    while (rawQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    while (recQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    REQUIRE(pool.inUse() == 0);
}

TEST_CASE("STEADY STATE: cero asignaciones con origenes intercalados y la tabla llena",
          "[net][receiver][allocations][steady][sources][RNF-013][F1.1-08][F1.1-09]")
{
    // Diez origenes que se turnan -ocho con indice y dos sin sitio- con tamanos
    // distintos y un truncado en cada vuelta. Tras una vuelta de calentamiento,
    // mil vueltas medidas: diez mil datagramas.
    std::vector<ScriptedDatagram> cycle;
    for (std::size_t i = 0; i < pw::net::kMaxSources + 2; ++i) {
        cycle.push_back({lanHost(i), i == 5 ? std::size_t{10} : 64 + i * 100});
    }
    constexpr std::size_t kRepeats = 1'001;

    pw::mem::BufferPool pool{pw::mem::kMinimumPoolSize, 2048};
    auto                rawQueue = std::make_unique<RawQueue>();
    auto                recQueue = std::make_unique<RecQueue>();

    pw::obs::MetricsRegistry metrics;
    pw::obs::Logger          logger{pw::obs::LoggerConfig{}};

    auto        scripted = std::make_unique<ScriptedSocket>(cycle, kRepeats);
    auto* const socket   = scripted.get();

    pw::net::UdpReceiver receiver{pw::net::UdpReceiverConfig{}, pool, *rawQueue, *recQueue,
                                  metrics, logger, std::move(scripted)};
    REQUIRE(receiver.open().has_value());

    const auto drain = [&] {
        ReceivedDatagram datagram;
        while (rawQueue->tryPop(datagram)) { pool.release(datagram.handle); }
        while (recQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    };

    // ── Calentamiento: una vuelta del guion, sin contar ──────────────────────
    socket->allowUpTo(cycle.size());
    std::stop_source warmUpStop;
    std::thread      warmUp{[&] { receiver.run(warmUpStop.get_token()); }};
    REQUIRE(waitUntil([&] {
        drain();
        return socket->delivered() == cycle.size();
    }));
    warmUpStop.request_stop();
    warmUp.join();

    alloc_probe::reset();
    const auto before = receiver.stats().received;

    // ── Medicion ────────────────────────────────────────────────────────────
    socket->allowUpTo(socket->total());
    std::stop_source measuredStop;
    std::thread      measured{[&] {
        alloc_probe::counting = true;
        receiver.run(measuredStop.get_token());
        alloc_probe::counting = false;
    }};
    REQUIRE(waitUntil([&] {
        drain();
        return socket->delivered() == socket->total();
    }));
    measuredStop.request_stop();
    measured.join();
    drain();

    const auto datagrams   = receiver.stats().received - before;
    const auto allocations = alloc_probe::count();

    WARN("STEADY origenes: " << datagrams << " datagramas de 10 origenes (8 registrados, 2 sin "
                             << "sitio) con " << allocations << " asignaciones en T1  ["
                             << alloc_probe::describeSizes() << "]");

    REQUIRE(datagrams >= 10'000);
    REQUIRE(receiver.sources().size() == pw::net::kMaxSources);
    REQUIRE(allocations == 0);
    REQUIRE(pool.inUse() == 0);
}

TEST_CASE("La instrumentacion detecta una asignacion de verdad",
          "[net][receiver][allocations]")
{
    // Control positivo. Sin esto, un contador que no cuenta nada daria cero y la
    // prueba pasaria igual, que es la forma mas comoda de no demostrar nada.
    std::atomic<std::uint64_t> observed{0};

    std::thread worker{[&] {
        alloc_probe::reset();
        alloc_probe::counting = true;

        auto* deliberate = new int[32];
        deliberate[0]    = 1;
        delete[] deliberate;

        observed.store(alloc_probe::count(), std::memory_order_release);
        alloc_probe::counting = false;
    }};
    worker.join();

    REQUIRE(observed.load(std::memory_order_acquire) >= 1);
}
