// Pruebas de la tabla de origenes (F1.1-09, RF-009) y de los contadores por
// origen y por tamano (F1.1-08, RF-008).
//
// Primero la tabla sola, sin sockets: identidad, capacidad, tabla llena y
// lectura desde otro hilo. Despues el receptor con un socket de guion, que es
// la unica forma de elegir la IP del emisor y no solo su puerto.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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
#include "pw/net/datagram_size_counts.hpp"
#include "pw/net/source_registry.hpp"
#include "pw/net/udp_receiver.hpp"
#include "pw/obs/logging.hpp"
#include "pw/obs/metrics.hpp"
#include "deterministic_priority.hpp"
#include "scripted_socket.hpp"

using namespace std::chrono_literals;

using pw::net::DatagramSizeCounts;
using pw::net::Ipv4Endpoint;
using pw::net::kMaxSources;
using pw::net::kUnregisteredSource;
using pw::net::ReceivedDatagram;
using pw::net::SourceIndex;
using pw::net::SourceRegistry;
using pw::test::ScriptedDatagram;
using pw::test::ScriptedSocket;

namespace {

constexpr Ipv4Endpoint endpoint(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d,
                                std::uint16_t port)
{
    return Ipv4Endpoint{{a, b, c, d}, port};
}

/// Origenes distintos por IP, todos al mismo puerto: 192.168.1.(10+i):20777.
constexpr Ipv4Endpoint lanHost(const std::size_t i)
{
    return endpoint(192, 168, 1, static_cast<std::uint8_t>(10 + i), 20777);
}

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

const pw::obs::MetricSample* findSample(const std::vector<pw::obs::MetricSample>& samples,
                                        const std::string& name, const std::string& labels)
{
    for (const auto& sample : samples) {
        if (sample.name == name && sample.labels == labels) { return &sample; }
    }
    return nullptr;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Identidad
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("El mismo IP y puerto obtiene siempre el mismo indice", "[net][sources][RF-009][F1.1-09]")
{
    SourceRegistry sources;
    const auto     game = endpoint(192, 168, 1, 50, 20777);

    const auto first = sources.resolve(game);
    REQUIRE(first == SourceIndex{0});
    for (int i = 0; i < 100; ++i) { REQUIRE(sources.resolve(game) == first); }
    REQUIRE(sources.size() == 1);
    REQUIRE(sources.endpoint(first) == game);
}

TEST_CASE("Misma IP con otro puerto es otro origen", "[net][sources][RF-009][F1.1-09]")
{
    SourceRegistry sources;
    const auto     a = sources.resolve(endpoint(192, 168, 1, 50, 20777));
    const auto     b = sources.resolve(endpoint(192, 168, 1, 50, 20778));

    REQUIRE(a != b);
    REQUIRE(sources.size() == 2);
    REQUIRE(sources.endpoint(a).port() == 20777);
    REQUIRE(sources.endpoint(b).port() == 20778);
}

TEST_CASE("Otra IP con el mismo puerto es otro origen", "[net][sources][RF-009][F1.1-09]")
{
    SourceRegistry sources;
    const auto     a = sources.resolve(endpoint(192, 168, 1, 50, 20777));
    const auto     b = sources.resolve(endpoint(192, 168, 1, 51, 20777));

    REQUIRE(a != b);
    REQUIRE(sources.endpoint(a) == endpoint(192, 168, 1, 50, 20777));
    REQUIRE(sources.endpoint(b) == endpoint(192, 168, 1, 51, 20777));
}

TEST_CASE("La clave binaria distingue todos los casos frontera", "[net][sources][F1.1-09]")
{
    // 0.0.0.0:0 es un origen representable y no se confunde con una ranura vacia.
    STATIC_REQUIRE(SourceRegistry::keyOf(Ipv4Endpoint{}) != 0);

    // IP y puerto no se solapan en la clave: intercambiar bits entre ellos da
    // otra clave.
    STATIC_REQUIRE(SourceRegistry::keyOf(endpoint(0, 0, 0, 1, 0))
                   != SourceRegistry::keyOf(endpoint(0, 0, 0, 0, 1)));
    STATIC_REQUIRE(SourceRegistry::keyOf(endpoint(255, 255, 255, 255, 0))
                   != SourceRegistry::keyOf(endpoint(0, 0, 0, 0, 0xFFFF)));

    // Ida y vuelta exacta.
    constexpr auto extremes = endpoint(255, 0, 128, 1, 65535);
    STATIC_REQUIRE(SourceRegistry::endpointOf(SourceRegistry::keyOf(extremes)) == extremes);

    SourceRegistry sources;
    const auto     any = sources.resolve(Ipv4Endpoint{});
    REQUIRE(any == SourceIndex{0});
    REQUIRE(sources.resolve(Ipv4Endpoint{}) == any);
    REQUIRE(sources.size() == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Contadores por origen
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Varios datagramas del mismo origen se acumulan en su indice",
          "[net][sources][RF-008][F1.1-08]")
{
    SourceRegistry sources;
    const auto     game = sources.resolve(endpoint(10, 0, 0, 2, 20777));

    for (int i = 0; i < 1000; ++i) { sources.record(game, 1349); }

    REQUIRE(sources.datagrams(game) == 1000);
    REQUIRE(sources.bytes(game) == 1000ULL * 1349);
    REQUIRE(sources.datagrams(kUnregisteredSource) == 0);
}

TEST_CASE("Origenes intercalados: indices estables y contadores separados",
          "[net][sources][RF-008][RF-009][F1.1-08][F1.1-09]")
{
    SourceRegistry sources;
    const auto     game    = endpoint(192, 168, 1, 50, 20777);
    const auto     console = endpoint(192, 168, 1, 60, 20777);
    const auto     tool    = endpoint(192, 168, 1, 50, 30000);

    // A B A C B A A C ...: el orden de aparicion fija los indices.
    const std::array<Ipv4Endpoint, 8> sequence{game, console, game, tool, console, game, game, tool};
    const std::array<std::size_t, 8>  sizes{1349, 1285, 1349, 45, 1285, 1349, 1349, 45};

    for (int round = 0; round < 50; ++round) {
        for (std::size_t i = 0; i < sequence.size(); ++i) {
            const auto index = sources.resolve(sequence[i]);
            sources.record(index, sizes[i]);
        }
    }

    const auto gameIndex    = sources.resolve(game);
    const auto consoleIndex = sources.resolve(console);
    const auto toolIndex    = sources.resolve(tool);

    REQUIRE(gameIndex == SourceIndex{0});
    REQUIRE(consoleIndex == SourceIndex{1});
    REQUIRE(toolIndex == SourceIndex{2});
    REQUIRE(sources.size() == 3);

    // Datagramas por origen: 4, 2 y 2 por ronda.
    REQUIRE(sources.datagrams(gameIndex) == 200);
    REQUIRE(sources.datagrams(consoleIndex) == 100);
    REQUIRE(sources.datagrams(toolIndex) == 100);

    // Bytes por origen, sin mezclarse.
    REQUIRE(sources.bytes(gameIndex) == 200ULL * 1349);
    REQUIRE(sources.bytes(consoleIndex) == 100ULL * 1285);
    REQUIRE(sources.bytes(toolIndex) == 100ULL * 45);
}

// ─────────────────────────────────────────────────────────────────────────────
// Capacidad y tabla llena
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("La tabla admite exactamente kMaxSources origenes", "[net][sources][F1.1-09]")
{
    STATIC_REQUIRE(SourceRegistry::capacity() == kMaxSources);
    STATIC_REQUIRE(kMaxSources == 8);

    SourceRegistry sources;
    for (std::size_t i = 0; i < kMaxSources; ++i) {
        REQUIRE(sources.resolve(lanHost(i)) == SourceIndex{static_cast<std::uint8_t>(i)});
    }
    REQUIRE(sources.size() == kMaxSources);
}

TEST_CASE("Con la tabla llena un origen nuevo se cuenta aparte y nada existente cambia",
          "[net][sources][F1.1-09]")
{
    SourceRegistry sources;
    for (std::size_t i = 0; i < kMaxSources; ++i) {
        const auto index = sources.resolve(lanHost(i));
        sources.record(index, 100 + i);
    }
    const auto before = sources.snapshot();

    // Dos origenes nuevos, varias veces cada uno.
    const auto ninth = endpoint(10, 0, 0, 9, 20777);
    const auto tenth = endpoint(10, 0, 0, 10, 20777);
    for (int i = 0; i < 5; ++i) {
        const auto a = sources.resolve(ninth);
        const auto b = sources.resolve(tenth);
        REQUIRE(a == kUnregisteredSource);
        REQUIRE(b == kUnregisteredSource);
        sources.record(a, 1000);
        sources.record(b, 10);
    }

    // No se ha registrado nada nuevo.
    REQUIRE(sources.size() == kMaxSources);

    // El desbordamiento esta contado, no perdido.
    REQUIRE(sources.datagrams(kUnregisteredSource) == 10);
    REQUIRE(sources.bytes(kUnregisteredSource) == 5 * 1000 + 5 * 10);

    // Ningun indice existente ha cambiado: mismo endpoint, mismo indice, mismos
    // contadores.
    const auto after = sources.snapshot();
    REQUIRE(after.size() == before.size());
    for (std::size_t i = 0; i < kMaxSources; ++i) {
        REQUIRE(after[i].index == before[i].index);
        REQUIRE(after[i].endpoint == before[i].endpoint);
        REQUIRE(after[i].datagrams == before[i].datagrams);
        REQUIRE(after[i].bytes == before[i].bytes);
        REQUIRE(sources.resolve(lanHost(i)) == SourceIndex{static_cast<std::uint8_t>(i)});
    }

    // Un origen conocido sigue contando en su indice con la tabla llena.
    sources.record(sources.resolve(lanHost(3)), 7);
    REQUIRE(sources.datagrams(SourceIndex{3}) == before[3].datagrams + 1);
}

TEST_CASE("Ningun indice se repite ni apunta a un origen equivocado", "[net][sources][F1.1-09]")
{
    SourceRegistry sources;
    // Muchos intentos sobre mas origenes que capacidad, en un orden que mezcla
    // conocidos y nuevos.
    for (int round = 0; round < 20; ++round) {
        for (std::size_t i = 0; i < kMaxSources + 4; ++i) {
            const auto origin = lanHost((i * 5 + static_cast<std::size_t>(round)) % (kMaxSources + 4));
            const auto index  = sources.resolve(origin);
            if (index != kUnregisteredSource) { REQUIRE(sources.endpoint(index) == origin); }
        }
    }

    const auto               all = sources.snapshot();
    std::set<std::uint8_t>   indices;
    std::set<std::uint64_t>  keys;
    for (std::size_t i = 0; i < all.size(); ++i) {
        REQUIRE(all[i].index == SourceIndex{static_cast<std::uint8_t>(i)});
        indices.insert(all[i].index.get());
        keys.insert(SourceRegistry::keyOf(all[i].endpoint));
    }
    REQUIRE(all.size() == kMaxSources);
    REQUIRE(indices.size() == kMaxSources);
    REQUIRE(keys.size() == kMaxSources);
}

// ─────────────────────────────────────────────────────────────────────────────
// Lectura desde fuera de T1
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Un lector concurrente ve siempre una tabla coherente", "[net][sources][F1.1-09]")
{
    // El escritor hace de T1: registra los ocho origenes en un orden conocido
    // mientras cuenta datagramas sin parar. El lector, desde otro hilo, toma
    // instantaneas y comprueba que cada una es coherente:
    //
    //   - los indices son 0..n-1, sin huecos ni repeticiones;
    //   - cada indice apunta al endpoint que el escritor le dio, y a ningun otro
    //     (una clave a medio escribir no puede verse);
    //   - el tamano y los contadores nunca retroceden.
    //
    // Esto no sustituye a ThreadSanitizer, que sigue pendiente: demuestra el
    // contrato observable, no la ausencia de carreras.
    SourceRegistry sources;
    std::atomic<bool> done{false};

    std::thread writer{[&] {
        for (std::size_t round = 0; round < 20'000; ++round) {
            const auto registered = std::min<std::size_t>(kMaxSources, 1 + round / 2'000);
            const auto origin     = lanHost(round % registered);
            sources.record(sources.resolve(origin), 64);
        }
        done.store(true, std::memory_order_release);
    }};

    std::size_t                             snapshots = 0;
    std::size_t                             lastSize  = 0;
    std::array<std::uint64_t, kMaxSources>  lastCounts{};
    bool                                    coherent = true;

    while (!done.load(std::memory_order_acquire) || snapshots == 0) {
        const auto view = sources.snapshot();
        ++snapshots;
        if (view.size() < lastSize) { coherent = false; }
        lastSize = view.size();
        for (std::size_t i = 0; i < view.size(); ++i) {
            if (view[i].index != SourceIndex{static_cast<std::uint8_t>(i)}) { coherent = false; }
            if (view[i].endpoint != lanHost(i)) { coherent = false; }
            if (view[i].datagrams < lastCounts[i]) { coherent = false; }
            lastCounts[i] = view[i].datagrams;
        }
    }
    writer.join();

    INFO("instantaneas tomadas durante la escritura: " << snapshots);
    REQUIRE(coherent);
    REQUIRE(sources.size() == kMaxSources);

    std::uint64_t total = 0;
    for (const auto& source : sources.snapshot()) { total += source.datagrams; }
    REQUIRE(total == 20'000);
}

TEST_CASE("Las metricas por origen llevan el texto del origen, construido fuera de T1",
          "[net][sources][RF-008][F1.1-08]")
{
    SourceRegistry sources;
    sources.record(sources.resolve(endpoint(192, 168, 1, 50, 20777)), 1349);
    sources.record(sources.resolve(endpoint(192, 168, 1, 50, 20777)), 1349);
    sources.record(sources.resolve(endpoint(127, 0, 0, 1, 51000)), 45);

    std::vector<pw::obs::MetricSample> samples;
    pw::net::appendMetricSamples(sources, samples);

    const auto* gamePackets =
        findSample(samples, "udp_packets_by_source_total", "source=192.168.1.50:20777");
    const auto* gameBytes =
        findSample(samples, "udp_bytes_by_source_total", "source=192.168.1.50:20777");
    const auto* local = findSample(samples, "udp_packets_by_source_total", "source=127.0.0.1:51000");
    const auto* unregistered =
        findSample(samples, "udp_packets_by_source_total", "source=unregistered");

    REQUIRE(gamePackets != nullptr);
    REQUIRE(gamePackets->count == 2);
    REQUIRE(gameBytes != nullptr);
    REQUIRE(gameBytes->count == 2 * 1349);
    REQUIRE(local != nullptr);
    REQUIRE(local->count == 1);
    // El desbordamiento se emite siempre, tambien a cero: «no se ha llenado» es
    // un dato, no una ausencia.
    REQUIRE(unregistered != nullptr);
    REQUIRE(unregistered->count == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Contadores por tamano
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Los contadores por tamano son exactos por longitud", "[net][sizes][RF-008][F1.1-08]")
{
    DatagramSizeCounts sizes{2048};
    REQUIRE(sizes.maxBytes() == 2048);

    for (int i = 0; i < 3; ++i) { sizes.record(1349); }
    sizes.record(1285);
    sizes.record(10);     // un truncado tambien tiene longitud observada
    sizes.record(0);
    sizes.record(2048);   // el limite exacto cabe

    REQUIRE(sizes.count(1349) == 3);
    REQUIRE(sizes.count(1285) == 1);
    REQUIRE(sizes.count(1348) == 0);   // la vecina no se contamina
    REQUIRE(sizes.count(10) == 1);
    REQUIRE(sizes.count(0) == 1);
    REQUIRE(sizes.count(2048) == 1);
    REQUIRE(sizes.outOfRange() == 0);

    sizes.record(2049);   // fuera de la tabla: se cuenta aparte, no se pierde
    REQUIRE(sizes.outOfRange() == 1);
    REQUIRE(sizes.count(2049) == 0);

    const auto observed = sizes.snapshot();
    REQUIRE(observed.size() == 5);
    REQUIRE(std::is_sorted(observed.begin(), observed.end(),
                           [](const auto& a, const auto& b) { return a.bytes < b.bytes; }));

    std::vector<pw::obs::MetricSample> samples;
    pw::net::appendMetricSamples(sizes, samples);
    REQUIRE(samples.size() == 5);
    const auto* motion = findSample(samples, "udp_packets_by_size_total", "bytes=1349");
    REQUIRE(motion != nullptr);
    REQUIRE(motion->count == 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// El receptor, con un socket de guion
// ─────────────────────────────────────────────────────────────────────────────

namespace {

using ScriptRawQueue = pw::mem::SpscRing<ReceivedDatagram, 64>;
using ScriptRecQueue = pw::mem::SpscRing<ReceivedDatagram, 64>;
using ScriptReceiver = pw::net::UdpReceiverT<ScriptRawQueue, ScriptRecQueue>;

/// Ejecuta un guion completo en el receptor y lo para. Las colas son de 64: el
/// guion cabe entero, asi que no hay descartes que confundan los recuentos.
struct ScriptRun {
    explicit ScriptRun(const std::vector<ScriptedDatagram>& script)
        : pool{256, 2048}
        , raw{std::make_unique<ScriptRawQueue>()}
        , rec{std::make_unique<ScriptRecQueue>()}
        , logger{pw::obs::LoggerConfig{}}
    {
        auto scripted = std::make_unique<ScriptedSocket>(script);
        socket        = scripted.get();
        receiver      = std::make_unique<ScriptReceiver>(pw::net::UdpReceiverConfig{}, pool, *raw,
                                                    *rec, metrics, logger, std::move(scripted),
                                                    &pw::monotonicNowNs, &pw::test::fakePriorityRaised);
        REQUIRE(receiver->open().has_value());

        std::stop_source stopper;
        std::thread      worker{[&] { receiver->run(stopper.get_token()); }};
        const auto all = waitUntil([&] { return socket->delivered() == socket->total(); });
        stopper.request_stop();
        worker.join();
        REQUIRE(all);
    }

    ~ScriptRun()
    {
        ReceivedDatagram datagram;
        while (raw->tryPop(datagram)) { pool.release(datagram.handle); }
        while (rec->tryPop(datagram)) { pool.release(datagram.handle); }
    }

    pw::mem::BufferPool             pool;
    std::unique_ptr<ScriptRawQueue> raw;
    std::unique_ptr<ScriptRecQueue> rec;
    pw::obs::MetricsRegistry        metrics;
    pw::obs::Logger                 logger;
    ScriptedSocket*                 socket{nullptr};
    std::unique_ptr<ScriptReceiver> receiver;
};

} // namespace

TEST_CASE("El receptor cuenta por origen y por tamano, y marca cada datagrama con su indice",
          "[net][receiver][sources][RF-008][RF-009][F1.1-08][F1.1-09]")
{
    const auto game    = endpoint(192, 168, 1, 50, 20777);
    const auto sameIp  = endpoint(192, 168, 1, 50, 20778);
    const auto samePort = endpoint(192, 168, 1, 51, 20777);

    const std::vector<ScriptedDatagram> script{
        {game, 1349},  {sameIp, 100},  {game, 1349}, {samePort, 200},
        {game, 10},    // truncado: cuenta en su origen y en su tamano
        {sameIp, 100}, {game, 1349},
        {game, 3000},  // oversized: el socket no informa del origen en ese error
    };

    ScriptRun run{script};
    const auto& receiver = *run.receiver;
    const auto& sources  = receiver.sources();

    REQUIRE(sources.size() == 3);
    REQUIRE(sources.endpoint(SourceIndex{0}) == game);
    REQUIRE(sources.endpoint(SourceIndex{1}) == sameIp);
    REQUIRE(sources.endpoint(SourceIndex{2}) == samePort);

    REQUIRE(sources.datagrams(SourceIndex{0}) == 4);
    REQUIRE(sources.bytes(SourceIndex{0}) == 3 * 1349 + 10);
    REQUIRE(sources.datagrams(SourceIndex{1}) == 2);
    REQUIRE(sources.bytes(SourceIndex{1}) == 200);
    REQUIRE(sources.datagrams(SourceIndex{2}) == 1);
    REQUIRE(sources.bytes(SourceIndex{2}) == 200);

    // Los globales siguen funcionando, y los desgloses suman lo mismo que ellos.
    const auto stats = receiver.stats();
    REQUIRE(stats.received == script.size() - 1);   // el oversized no se llega a leer
    REQUIRE(stats.truncated == 1);
    REQUIRE(stats.oversized == 1);
    std::uint64_t perSourceTotal = sources.datagrams(kUnregisteredSource);
    std::uint64_t perSourceBytes = sources.bytes(kUnregisteredSource);
    for (const auto& source : sources.snapshot()) {
        perSourceTotal += source.datagrams;
        perSourceBytes += source.bytes;
    }
    REQUIRE(perSourceTotal == stats.received);
    REQUIRE(perSourceBytes == stats.bytes);

    const auto& sizes = receiver.sizeCounts();
    REQUIRE(sizes.maxBytes() == 2048);   // la tabla se dimensiona con el buffer del pool
    REQUIRE(sizes.count(1349) == 3);
    REQUIRE(sizes.count(100) == 2);
    REQUIRE(sizes.count(200) == 1);
    REQUIRE(sizes.count(10) == 1);
    REQUIRE(sizes.count(3000) == 0);
    REQUIRE(sizes.outOfRange() == 0);

    // Cada datagrama encolado lleva el indice de SU origen, y su origen exacto.
    std::size_t      queued = 0;
    ReceivedDatagram datagram;
    while (run.raw->tryPop(datagram)) {
        ++queued;
        REQUIRE(datagram.source != kUnregisteredSource);
        REQUIRE(sources.endpoint(datagram.source) == datagram.origin);
        run.pool.release(datagram.handle);
    }
    REQUIRE(queued == 6);   // 7 leidos menos el truncado; el oversized no se leyo
}

TEST_CASE("Con la tabla llena el receptor sigue entregando, cuenta aparte y lo avisa fuera de T1",
          "[net][receiver][sources][F1.1-09]")
{
    std::vector<ScriptedDatagram> script;
    for (std::size_t i = 0; i < kMaxSources; ++i) { script.push_back({lanHost(i), 100}); }
    const auto ninth = endpoint(10, 0, 0, 9, 20777);
    script.push_back({ninth, 300});
    script.push_back({lanHost(0), 100});   // uno conocido, con la tabla ya llena
    script.push_back({ninth, 300});

    ScriptRun   run{script};
    const auto& receiver = *run.receiver;
    const auto& sources  = receiver.sources();

    REQUIRE(sources.size() == kMaxSources);
    REQUIRE(receiver.eventSeen(pw::net::ReceiverEvent::SourceTableFull));
    REQUIRE(sources.datagrams(kUnregisteredSource) == 2);
    REQUIRE(sources.bytes(kUnregisteredSource) == 600);
    REQUIRE(sources.datagrams(SourceIndex{0}) == 2);

    // La tabla no filtra: todo lo valido se ha entregado.
    REQUIRE(receiver.stats().delivered == script.size());

    std::size_t      unregistered = 0;
    ReceivedDatagram datagram;
    while (run.raw->tryPop(datagram)) {
        if (datagram.source == kUnregisteredSource) {
            ++unregistered;
            REQUIRE(datagram.origin == ninth);   // el origen exacto no se pierde
        } else {
            REQUIRE(sources.endpoint(datagram.source) == datagram.origin);
        }
        run.pool.release(datagram.handle);
    }
    REQUIRE(unregistered == 2);

    // El aviso lo emite quien esta fuera de T1, una vez.
    REQUIRE(run.receiver->reportPendingEvents() == 1);
    REQUIRE(run.receiver->reportPendingEvents() == 0);
}
