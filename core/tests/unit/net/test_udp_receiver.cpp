// Pruebas del bucle de recepcion (F1.1-05, F1.1-06, F1.1-07 y sus contadores).
//
// El foco esta en el reparto y en los caminos de error: que ningun BufferHandle
// se fugue, que una cola llena no afecte a la otra, y que los contadores sean
// del componente y no del test.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
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

using namespace std::chrono_literals;

using pw::net::Ipv4Endpoint;
using pw::net::kLoopbackAddress;
using pw::net::ReceivedDatagram;

namespace {

/// Colas diminutas para poder llenarlas de verdad en una prueba.
constexpr std::size_t kTestQueueCapacity = 4;

using TestRawQueue = pw::mem::SpscRing<ReceivedDatagram, kTestQueueCapacity>;
using TestRecQueue = pw::mem::SpscRing<ReceivedDatagram, kTestQueueCapacity>;
using TestReceiver = pw::net::UdpReceiverT<TestRawQueue, TestRecQueue>;

template <typename Predicate>
bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout = 3000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) { return true; }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

/// Monta receptor, pool, colas, metricas y un emisor, y lo desmonta ordenado.
class Harness {
public:
    explicit Harness(const std::size_t poolBuffers = 64,
                     const std::size_t bufferBytes = 2048)
        : pool_{poolBuffers, bufferBytes}
        , rawQueue_{std::make_unique<TestRawQueue>()}
        , recQueue_{std::make_unique<TestRecQueue>()}
        , logger_{quietLogger()}
    {
        pw::net::UdpReceiverConfig config;
        config.bindAddress            = "127.0.0.1";
        config.port                   = 0;      // que el sistema conceda uno
        config.pollTimeoutMs          = 10;     // parada rapida en las pruebas
        config.poolExhaustedBackoffUs = 200;
        config.receiveBufferBytes     = 4 * 1024 * 1024;

        receiver_ = std::make_unique<TestReceiver>(config, pool_, *rawQueue_, *recQueue_,
                                                   metrics_, logger_, pw::net::makeUdpSocket(), &pw::monotonicNowNs, &pw::test::fakePriorityRaised);

        const auto opened = receiver_->open();
        REQUIRE(opened.has_value());
        target_ = opened.value();
        REQUIRE(target_.port() != 0);

        sender_ = pw::net::makeUdpSocket();
        REQUIRE(sender_->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());
    }

    ~Harness() { stop(); }

    Harness(const Harness&)            = delete;
    Harness& operator=(const Harness&) = delete;

    void start()
    {
        worker_ = std::thread{[this] { receiver_->run(stopper_.get_token()); }};
    }

    void stop()
    {
        stopper_.request_stop();
        if (worker_.joinable()) { worker_.join(); }
    }

    void send(const std::size_t size, const std::byte fill = std::byte{0xA5})
    {
        const std::vector<std::byte> payload(size, fill);
        REQUIRE(sender_->sendTo(target_, payload).has_value());
    }

    void send(const std::vector<std::byte>& payload)
    {
        REQUIRE(sender_->sendTo(target_, payload).has_value());
    }

    /// Vacia una cola soltando las referencias, como haria su consumidor.
    std::size_t drain(TestRawQueue& queue)
    {
        std::size_t      drained = 0;
        ReceivedDatagram datagram;
        while (queue.tryPop(datagram)) {
            ++drained;
            pool_.release(datagram.handle);
        }
        return drained;
    }

    [[nodiscard]] pw::mem::BufferPool& pool() noexcept { return pool_; }
    [[nodiscard]] TestRawQueue&        raw()  noexcept { return *rawQueue_; }
    [[nodiscard]] TestRecQueue&        rec()  noexcept { return *recQueue_; }
    [[nodiscard]] TestReceiver&        receiver() noexcept { return *receiver_; }
    [[nodiscard]] pw::obs::MetricsRegistry& metrics() noexcept { return metrics_; }
    [[nodiscard]] Ipv4Endpoint         senderEndpoint() const { return sender_->localEndpoint(); }

private:
    static pw::obs::LoggerConfig quietLogger()
    {
        pw::obs::LoggerConfig config;
        config.level               = pw::obs::LogLevel::Info;
        config.aggregationWindowMs = 60'000;
        return config;   // sin destinos: no escribe en ningun sitio
    }

    pw::mem::BufferPool           pool_;
    std::unique_ptr<TestRawQueue> rawQueue_;
    std::unique_ptr<TestRecQueue> recQueue_;
    pw::obs::MetricsRegistry      metrics_;
    pw::obs::Logger               logger_;
    std::unique_ptr<TestReceiver> receiver_;
    std::unique_ptr<pw::net::IUdpSocket> sender_;
    Ipv4Endpoint                  target_{};
    std::stop_source              stopper_;
    std::thread                   worker_;
};

/// Destino de log en memoria, para poder mirar que se registro y que no.
class MemorySink final : public pw::obs::ILogSink {
public:
    void write(const std::string_view line) override
    {
        const std::lock_guard lock{mutex_};
        lines_.emplace_back(line);
    }
    void flush() override {}

    [[nodiscard]] std::vector<std::string> lines() const
    {
        const std::lock_guard lock{mutex_};
        return lines_;
    }

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

} // namespace

TEST_CASE("El receptor se enlaza y registra el buffer efectivo",
          "[net][receiver][F1.1-05][F1.1-06]")
{
    Harness harness;

    REQUIRE(harness.receiver().localEndpoint().port() != 0);
    REQUIRE(harness.receiver().effectiveReceiveBufferBytes() > 0);

    // El valor efectivo, no el solicitado, queda publicado como metrica.
    const auto samples = harness.metrics().snapshot();
    bool       found   = false;
    for (const auto& sample : samples) {
        if (sample.name == "udp_socket_receive_buffer_bytes") {
            found = true;
            REQUIRE(sample.gauge ==
                    static_cast<std::int64_t>(harness.receiver().effectiveReceiveBufferBytes()));
        }
    }
    REQUIRE(found);

    WARN("SO_RCVBUF efectivo: " << harness.receiver().effectiveReceiveBufferBytes() << " B");
}

TEST_CASE("Una direccion de escucha invalida se rechaza antes de tocar el socket",
          "[net][receiver][RNF-097]")
{
    pw::mem::BufferPool      pool{8};
    auto                     raw = std::make_unique<TestRawQueue>();
    auto                     rec = std::make_unique<TestRecQueue>();
    pw::obs::MetricsRegistry metrics;
    pw::obs::Logger          logger{pw::obs::LoggerConfig{}};

    pw::net::UdpReceiverConfig config;
    config.bindAddress = "no-es-una-ip";

    TestReceiver receiver{config, pool, *raw, *rec, metrics, logger,
                          pw::net::makeUdpSocket(), &pw::monotonicNowNs, &pw::test::fakePriorityRaised};
    const auto   opened = receiver.open();

    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().kind == pw::net::SocketErrorKind::InvalidAddress);
    REQUIRE(opened.error().describe().find("no-es-una-ip") != std::string::npos);
}

TEST_CASE("Un datagrama llega a las dos colas, intacto y con su marca de tiempo",
          "[net][receiver][F1.1-07][RF-001][RF-007]")
{
    Harness harness;

    std::vector<std::byte> payload(120);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i & 0xFF);
    }

    const auto beforeNs = pw::monotonicNowNs();
    harness.start();
    harness.send(payload);

    REQUIRE(waitUntil([&] { return harness.receiver().stats().delivered >= 1; }));
    harness.stop();
    const auto afterNs = pw::monotonicNowNs();

    // El mismo datagrama esta en las dos colas, y es UN SOLO buffer con dos
    // referencias: no se ha copiado el payload.
    ReceivedDatagram fromRaw;
    ReceivedDatagram fromRec;
    REQUIRE(harness.raw().tryPop(fromRaw));
    REQUIRE(harness.rec().tryPop(fromRec));
    REQUIRE(fromRaw.handle == fromRec.handle);
    REQUIRE(harness.pool().refCount(fromRaw.handle) == 2);

    // Byte por byte, sin alterarse.
    const auto bytes = harness.pool().payload(fromRaw.handle);
    REQUIRE(bytes.size() == payload.size());
    REQUIRE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);

    // Marca de reloj monotono dentro de la ventana de la prueba (RF-007).
    REQUIRE(fromRaw.recvTimeNs > beforeNs);
    REQUIRE(fromRaw.recvTimeNs < afterNs);
    REQUIRE(fromRaw.recvTimeNs == fromRec.recvTimeNs);

    // Y el origen es el emisor real (RF-009).
    REQUIRE(fromRaw.origin.isLoopback());
    REQUIRE(fromRaw.origin.port() == harness.senderEndpoint().port());

    harness.pool().release(fromRaw.handle);
    harness.pool().release(fromRec.handle);
    REQUIRE(harness.pool().inUse() == 0);
}

TEST_CASE("Un datagrama de 10 bytes se cuenta como truncado y no rompe nada",
          "[net][receiver][RF-004][RNF-023]")
{
    Harness harness;
    harness.start();

    harness.send(10);
    REQUIRE(waitUntil([&] { return harness.receiver().stats().truncated >= 1; }));

    // Y despues de uno truncado, el siguiente valido pasa perfectamente.
    harness.send(64);
    REQUIRE(waitUntil([&] { return harness.receiver().stats().delivered >= 1; }));
    harness.stop();

    const auto stats = harness.receiver().stats();
    REQUIRE(stats.received == 2);
    REQUIRE(stats.truncated == 1);
    REQUIRE(stats.delivered == 1);
    REQUIRE(stats.bytes == 74);

    // El buffer del truncado volvio al pool: no se fuga por el camino de error.
    REQUIRE(harness.drain(harness.raw()) == 1);
    ReceivedDatagram datagram;
    while (harness.rec().tryPop(datagram)) { harness.pool().release(datagram.handle); }
    REQUIRE(harness.pool().inUse() == 0);
}

TEST_CASE("Un datagrama mayor que el buffer se descarta y se cuenta",
          "[net][receiver][RNF-023]")
{
    Harness harness{16, 512};   // buffers pequenos, para provocarlo facil
    harness.start();

    harness.send(513);
    REQUIRE(waitUntil([&] { return harness.receiver().stats().oversized >= 1; }));
    harness.stop();

    REQUIRE(harness.receiver().stats().oversized == 1);
    REQUIRE(harness.receiver().stats().delivered == 0);
    REQUIRE(harness.pool().inUse() == 0);   // el buffer volvio al pool
}

TEST_CASE("Qraw llena descarta el mas nuevo y no toca a Qrec",
          "[net][receiver][F1.1-07][RNF-052]")
{
    Harness harness{64};
    harness.start();

    // Se envian mas datagramas que huecos tiene Qraw y NADIE la drena. Qrec si
    // se drena, para que la unica cola tensionada sea Qraw.
    constexpr int kDatagrams = static_cast<int>(kTestQueueCapacity) + 6;
    for (int i = 0; i < kDatagrams; ++i) {
        harness.send(64);
        std::this_thread::sleep_for(2ms);
        ReceivedDatagram datagram;
        while (harness.rec().tryPop(datagram)) { harness.pool().release(datagram.handle); }
    }

    // Hay que esperar a las DOS condiciones: que el socket este vaciado y que se
    // haya descartado algo. Esperar solo al primer descarte y parar deja
    // datagramas sin leer en el buffer del sistema, y entonces la prueba mide el
    // momento en que se paro, no lo que hizo el receptor.
    // Hay que esperar a las DOS condiciones: que el socket este vaciado y que se
    // haya descartado algo. Esperar solo al primer descarte y parar dejaria
    // datagramas sin leer, y entonces la prueba mediria el momento en que se
    // paro, no lo que hizo el receptor.
    REQUIRE(waitUntil([&] {
        ReceivedDatagram pending;
        while (harness.rec().tryPop(pending)) { harness.pool().release(pending.handle); }
        const auto current = harness.receiver().stats();
        return current.received >= kDatagrams && current.qrawDropped > 0;
    }));
    harness.stop();

    const auto stats = harness.receiver().stats();
    REQUIRE(stats.qrawDropped > 0);
    REQUIRE(stats.qrecDropped == 0);          // Qrec no se ha visto afectada
    REQUIRE(stats.received == kDatagrams);
    REQUIRE(stats.delivered == kDatagrams);   // todos llegaron al menos a una cola

    // Qraw conserva EXACTAMENTE su capacidad: descartar el mas nuevo no toca lo
    // que ya estaba dentro.
    REQUIRE(harness.raw().size() == kTestQueueCapacity);
    REQUIRE(harness.drain(harness.raw()) == kTestQueueCapacity);

    ReceivedDatagram datagram;
    while (harness.rec().tryPop(datagram)) { harness.pool().release(datagram.handle); }
    REQUIRE(harness.pool().inUse() == 0);     // ni una referencia colgando

    // El contador es del componente y esta publicado como metrica.
    REQUIRE(harness.metrics().counter("pipeline_dropped_total", "queue=raw,reason=full").value() ==
            stats.qrawDropped);
}

TEST_CASE("El grabador atascado no afecta al directo",
          "[net][receiver][F1.1-07][RNF-052]")
{
    // El escenario que decidio el dimensionado del pool, ahora con el receptor
    // real: Qrec no se drena nunca y Qraw si.
    Harness harness{64};
    harness.start();

    constexpr int kDatagrams = static_cast<int>(kTestQueueCapacity) + 8;
    std::size_t   toPipeline = 0;

    for (int i = 0; i < kDatagrams; ++i) {
        harness.send(64);
        std::this_thread::sleep_for(2ms);
        toPipeline += harness.drain(harness.raw());   // el pipeline va al dia
    }

    REQUIRE(waitUntil([&] {
        toPipeline += harness.drain(harness.raw());
        const auto current = harness.receiver().stats();
        return current.received >= kDatagrams && current.qrecDropped > 0;
    }));
    harness.stop();
    toPipeline += harness.drain(harness.raw());

    const auto stats = harness.receiver().stats();
    REQUIRE(stats.received == kDatagrams);
    REQUIRE(stats.qrawDropped == 0);                          // el directo, intacto
    REQUIRE(stats.qrecDropped > 0);                           // el atasco se queda en Qrec
    REQUIRE(toPipeline == static_cast<std::size_t>(kDatagrams));

    ReceivedDatagram datagram;
    while (harness.rec().tryPop(datagram)) { harness.pool().release(datagram.handle); }
    REQUIRE(harness.pool().inUse() == 0);
}

TEST_CASE("El agotamiento del pool se cuenta como estado, no como perdida",
          "[net][receiver][RNF-013]")
{
    // Un pool de dos buffers y nadie drenando: en cuanto las dos referencias se
    // quedan en las colas, no hay buffer para el siguiente intento.
    //
    // Lo que se comprueba aqui, ademas del recuento, es la SEMANTICA: cuando
    // acquire() falla todavia no se ha leido nada del socket, asi que no se
    // puede afirmar que se haya perdido un datagrama concreto. Puede seguir
    // esperando en el buffer del sistema y recibirse despues.
    Harness harness{2};
    harness.start();

    for (int i = 0; i < 8; ++i) {
        harness.send(64);
        std::this_thread::sleep_for(2ms);
    }

    REQUIRE(waitUntil([&] { return harness.receiver().stats().poolExhaustions > 0; }));
    harness.stop();

    const auto exhausted = harness.receiver().stats();
    REQUIRE(exhausted.poolExhaustions > 0);
    REQUIRE(harness.pool().stats().exhausted > 0);

    // La metrica se llama por lo que es: agotamientos del pool, no descartes.
    REQUIRE(harness.metrics().counter("udp_pool_exhaustion_total").value() ==
            exhausted.poolExhaustions);

    // Y en efecto, los datagramas que no se pudieron recibir NO estan contados
    // como recibidos ni como descartados por ninguna cola: simplemente no se
    // han leido todavia.
    REQUIRE(exhausted.received == exhausted.delivered + exhausted.truncated +
                                      exhausted.oversized);

    // Al drenar, todo vuelve: ni un handle perdido pese al camino degradado.
    harness.drain(harness.raw());
    ReceivedDatagram datagram;
    while (harness.rec().tryPop(datagram)) { harness.pool().release(datagram.handle); }
    REQUIRE(harness.pool().inUse() == 0);
    REQUIRE(harness.pool().available() == 2);
}

TEST_CASE("El bucle termina cuando se le pide parar", "[net][receiver][F1.1-12][RNF-055]")
{
    Harness harness;
    harness.start();

    harness.send(64);
    REQUIRE(waitUntil([&] { return harness.receiver().stats().received >= 1; }));

    const auto startedNs = pw::monotonicNowNs();
    harness.stop();                       // request_stop + join
    const auto elapsedMs = (pw::monotonicNowNs() - startedNs) / 1'000'000;

    // Con espera acotada de 10 ms, parar tiene que ser inmediato en la practica.
    REQUIRE(elapsedMs < 1000);

    harness.drain(harness.raw());
    ReceivedDatagram datagram;
    while (harness.rec().tryPop(datagram)) { harness.pool().release(datagram.handle); }
    REQUIRE(harness.pool().inUse() == 0);
}

TEST_CASE("Con el socket cerrado el bucle vuelve en vez de girar",
          "[net][receiver][F1.1-12]")
{
    Harness harness;
    harness.receiver().close();

    // Sin senal de parada: lo que tiene que hacer volver a run() es el socket
    // cerrado. Si no volviera, esta prueba se quedaria colgada y CTest lo
    // cortaria por tiempo, que es exactamente el fallo que hay que ver.
    std::stop_source  never;
    std::atomic<bool> returned{false};
    std::thread       worker{[&] {
        harness.receiver().run(never.get_token());
        returned.store(true, std::memory_order_release);
    }};

    REQUIRE(waitUntil([&] { return returned.load(std::memory_order_acquire); }, 2000ms));
    worker.join();

    REQUIRE(harness.pool().inUse() == 0);
}

TEST_CASE("Una rafaga se recibe entera con la configuracion de produccion",
          "[net][receiver][RNF-026]")
{
    // Esta prueba usa los TIPOS DE PRODUCCION: las colas de 8192 huecos de
    // 04-ARCHITECTURE 4.3 y un pool dimensionado por el modelo de propiedad. Es
    // la unica que instancia pw::net::UdpReceiver tal y como se usara de verdad.
    //
    // No es la prueba de carga de F1.1-14 -eso son 1000 datagramas/s durante
    // media hora-: aqui lo que se comprueba es que el camino completo no pierde
    // nada cuando nada esta infradimensionado.
    constexpr int kDatagrams = 200;

    pw::mem::BufferPool pool{pw::mem::kMinimumPoolSize, 2048};
    auto                rawQueue = std::make_unique<pw::net::RawDatagramQueue>();
    auto                recQueue = std::make_unique<pw::net::RecDatagramQueue>();

    pw::obs::MetricsRegistry metrics;
    pw::obs::LoggerConfig    loggerConfig;
    loggerConfig.aggregationWindowMs = 60'000;
    pw::obs::Logger logger{loggerConfig};

    pw::net::UdpReceiverConfig config;
    config.bindAddress   = "127.0.0.1";
    config.port          = 0;
    config.pollTimeoutMs = 10;

    pw::net::UdpReceiver receiver{config,  pool,   *rawQueue, *recQueue, metrics, logger,
                                  pw::net::makeUdpSocket(), &pw::monotonicNowNs, &pw::test::fakePriorityRaised};
    const auto           opened = receiver.open();
    REQUIRE(opened.has_value());

    auto sender = pw::net::makeUdpSocket();
    REQUIRE(sender->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());

    std::stop_source stopper;
    std::thread      worker{[&] { receiver.run(stopper.get_token()); }};

    const std::vector<std::byte> payload(1470, std::byte{0x3C});   // el paquete mayor de 2026
    for (int i = 0; i < kDatagrams; ++i) {
        REQUIRE(sender->sendTo(opened.value(), payload).has_value());
    }

    const auto arrived =
        waitUntil([&] { return receiver.stats().received >= kDatagrams; }, 5000ms);
    stopper.request_stop();
    worker.join();

    const auto stats = receiver.stats();
    REQUIRE(arrived);
    REQUIRE(stats.received == kDatagrams);
    REQUIRE(stats.delivered == kDatagrams);
    REQUIRE(stats.truncated == 0);
    REQUIRE(stats.oversized == 0);
    REQUIRE(stats.socketErrors == 0);

    // Con todo bien dimensionado no se descarta NADA por ningun motivo.
    REQUIRE(stats.qrawDropped == 0);
    REQUIRE(stats.qrecDropped == 0);
    REQUIRE(stats.poolExhaustions == 0);

    // Los 200 estan en las dos colas, cada uno con sus dos referencias.
    REQUIRE(rawQueue->size() == kDatagrams);
    REQUIRE(recQueue->size() == kDatagrams);

    std::size_t      toPipeline = 0;
    std::size_t      toRecorder = 0;
    ReceivedDatagram datagram;
    while (rawQueue->tryPop(datagram)) {
        REQUIRE(pool.payloadSize(datagram.handle) == payload.size());
        ++toPipeline;
        pool.release(datagram.handle);
    }
    while (recQueue->tryPop(datagram)) {
        ++toRecorder;
        pool.release(datagram.handle);
    }

    REQUIRE(toPipeline == kDatagrams);
    REQUIRE(toRecorder == kDatagrams);
    REQUIRE(pool.inUse() == 0);

    WARN("rafaga de " << kDatagrams << " datagramas de 1470 B con colas de "
                      << pw::mem::kQrawCapacity << " y pool de " << pw::mem::kMinimumPoolSize
                      << ": 0 descartes, SO_RCVBUF efectivo "
                      << receiver.effectiveReceiveBufferBytes() << " B");
}

// ─────────────────────────────────────────────────────────────────────────────
// EVENTOS: T1 marca, otro informa (F1.1-07, RNF-013)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("T1 marca el suceso y reportPendingEvents lo registra fuera de T1",
          "[net][receiver][events]")
{
    auto sink = std::make_shared<MemorySink>();

    pw::mem::BufferPool      pool{16, 512};
    auto                     rawQueue = std::make_unique<TestRawQueue>();
    auto                     recQueue = std::make_unique<TestRecQueue>();
    pw::obs::MetricsRegistry metrics;

    pw::obs::LoggerConfig loggerConfig;
    loggerConfig.level               = pw::obs::LogLevel::Info;
    loggerConfig.aggregationWindowMs = 0;
    pw::obs::Logger logger{loggerConfig};
    logger.addSink(sink);

    pw::net::UdpReceiverConfig config;
    config.bindAddress   = "127.0.0.1";
    config.port          = 0;
    config.pollTimeoutMs = 10;

    TestReceiver receiver{config,  pool,   *rawQueue, *recQueue, metrics, logger,
                          pw::net::makeUdpSocket(), &pw::monotonicNowNs, &pw::test::fakePriorityRaised};
    const auto   opened = receiver.open();
    REQUIRE(opened.has_value());

    auto sender = pw::net::makeUdpSocket();
    REQUIRE(sender->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());

    // Antes de nada, ningun suceso y nada que informar.
    REQUIRE_FALSE(receiver.eventSeen(pw::net::ReceiverEvent::Truncated));
    REQUIRE(receiver.reportPendingEvents() == 0);

    std::stop_source stopper;
    std::thread      worker{[&] { receiver.run(stopper.get_token()); }};

    // Un truncado: T1 lo detecta y lo marca, pero NO lo registra.
    const std::vector<std::byte> tiny(10, std::byte{0x01});
    REQUIRE(sender->sendTo(opened.value(), tiny).has_value());

    REQUIRE(waitUntil([&] { return receiver.eventSeen(pw::net::ReceiverEvent::Truncated); }));
    logger.flush();
    REQUIRE(sink->countContaining("longitud minima") == 0);   // T1 no ha escrito nada

    // Y ahora lo informa quien esta fuera de T1.
    REQUIRE(receiver.reportPendingEvents() == 1);
    logger.flush();
    REQUIRE(sink->countContaining("longitud minima") == 1);

    stopper.request_stop();
    worker.join();

    ReceivedDatagram datagram;
    while (rawQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    while (recQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    REQUIRE(pool.inUse() == 0);
}

TEST_CASE("Un suceso repetido mil veces se registra una sola vez",
          "[net][receiver][events][RNF-102]")
{
    auto sink = std::make_shared<MemorySink>();

    pw::mem::BufferPool      pool{16, 512};
    auto                     rawQueue = std::make_unique<TestRawQueue>();
    auto                     recQueue = std::make_unique<TestRecQueue>();
    pw::obs::MetricsRegistry metrics;

    pw::obs::LoggerConfig loggerConfig;
    loggerConfig.level               = pw::obs::LogLevel::Info;
    loggerConfig.aggregationWindowMs = 0;
    pw::obs::Logger logger{loggerConfig};
    logger.addSink(sink);

    pw::net::UdpReceiverConfig config;
    config.bindAddress   = "127.0.0.1";
    config.port          = 0;
    config.pollTimeoutMs = 10;

    TestReceiver receiver{config,  pool,   *rawQueue, *recQueue, metrics, logger,
                          pw::net::makeUdpSocket(), &pw::monotonicNowNs, &pw::test::fakePriorityRaised};
    const auto   opened = receiver.open();
    REQUIRE(opened.has_value());

    auto sender = pw::net::makeUdpSocket();
    REQUIRE(sender->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());

    std::stop_source stopper;
    std::thread      worker{[&] { receiver.run(stopper.get_token()); }};

    // Mil truncados: mil marcas de la MISMA bandera.
    const std::vector<std::byte> tiny(10, std::byte{0x01});
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(sender->sendTo(opened.value(), tiny).has_value());
    }
    REQUIRE(waitUntil([&] { return receiver.stats().truncated >= 500; }));

    // Y muchas llamadas al informador, por si acaso.
    std::size_t reportedTotal = 0;
    for (int i = 0; i < 20; ++i) { reportedTotal += receiver.reportPendingEvents(); }

    stopper.request_stop();
    worker.join();
    reportedTotal += receiver.reportPendingEvents();
    logger.flush();

    // Un suceso, una linea. El recuento exacto vive en los contadores, que es
    // donde tiene sentido mirarlo.
    REQUIRE(reportedTotal == 1);
    REQUIRE(sink->countContaining("longitud minima") == 1);
    REQUIRE(receiver.stats().truncated >= 500);

    ReceivedDatagram datagram;
    while (rawQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    while (recQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    REQUIRE(pool.inUse() == 0);
}

TEST_CASE("Varios sucesos distintos se informan cada uno una vez",
          "[net][receiver][events]")
{
    auto sink = std::make_shared<MemorySink>();

    // Colas de cuatro y pool dimensionado para que se llenen: asi ocurren
    // truncado, oversized, Qraw llena y Qrec llena.
    pw::mem::BufferPool      pool{pw::mem::poolSizeForQueues(kTestQueueCapacity * 2, 2), 512};
    auto                     rawQueue = std::make_unique<TestRawQueue>();
    auto                     recQueue = std::make_unique<TestRecQueue>();
    pw::obs::MetricsRegistry metrics;

    pw::obs::LoggerConfig loggerConfig;
    loggerConfig.level               = pw::obs::LogLevel::Info;
    loggerConfig.aggregationWindowMs = 0;
    pw::obs::Logger logger{loggerConfig};
    logger.addSink(sink);

    pw::net::UdpReceiverConfig config;
    config.bindAddress   = "127.0.0.1";
    config.port          = 0;
    config.pollTimeoutMs = 10;

    TestReceiver receiver{config,  pool,   *rawQueue, *recQueue, metrics, logger,
                          pw::net::makeUdpSocket(), &pw::monotonicNowNs, &pw::test::fakePriorityRaised};
    const auto   opened = receiver.open();
    REQUIRE(opened.has_value());

    auto sender = pw::net::makeUdpSocket();
    REQUIRE(sender->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());

    std::stop_source stopper;
    std::thread      worker{[&] { receiver.run(stopper.get_token()); }};

    const auto send = [&](const std::size_t bytes) {
        const std::vector<std::byte> payload(bytes, std::byte{0x33});
        REQUIRE(sender->sendTo(opened.value(), payload).has_value());
    };

    send(10);    // truncado
    send(600);   // oversized: el buffer del pool es de 512 B
    for (int i = 0; i < 12; ++i) { send(64); }   // llena las dos colas

    REQUIRE(waitUntil([&] {
        return receiver.eventSeen(pw::net::ReceiverEvent::Truncated)
               && receiver.eventSeen(pw::net::ReceiverEvent::Oversized)
               && receiver.eventSeen(pw::net::ReceiverEvent::QueueRawFull)
               && receiver.eventSeen(pw::net::ReceiverEvent::QueueRecFull);
    }));

    stopper.request_stop();
    worker.join();

    const auto reported = receiver.reportPendingEvents();
    logger.flush();

    REQUIRE(reported == 4);
    REQUIRE(sink->countContaining("longitud minima") == 1);
    REQUIRE(sink->countContaining("mayor que el buffer") == 1);
    REQUIRE(sink->countContaining("Qraw llena") == 1);
    REQUIRE(sink->countContaining("Qrec llena") == 1);

    // Una segunda llamada no repite ninguno.
    REQUIRE(receiver.reportPendingEvents() == 0);
    logger.flush();
    REQUIRE(sink->countContaining("longitud minima") == 1);

    ReceivedDatagram datagram;
    while (rawQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    while (recQueue->tryPop(datagram)) { pool.release(datagram.handle); }
    REQUIRE(pool.inUse() == 0);
}
