// Pruebas de las metricas internas (F0-08, RF-800, OBS-01).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>
#include <vector>

#include "pw/obs/metrics.hpp"

using pw::obs::Counter;
using pw::obs::Gauge;
using pw::obs::Histogram;
using pw::obs::MetricKind;
using pw::obs::MetricsRegistry;

TEST_CASE("Un contador solo crece", "[obs][metrics][RF-800]")
{
    Counter packets;
    REQUIRE(packets.value() == 0);

    packets.inc();
    packets.inc(414);
    REQUIRE(packets.value() == 415);
}

TEST_CASE("Un contador es seguro entre hilos", "[obs][metrics][RNF-100]")
{
    Counter                  received;
    constexpr int            kThreads       = 4;
    constexpr int            kPerThread     = 50'000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&received] {
            for (int i = 0; i < kPerThread; ++i) { received.inc(); }
        });
    }
    for (auto& thread : threads) { thread.join(); }

    REQUIRE(received.value() == static_cast<std::uint64_t>(kThreads) * kPerThread);
}

TEST_CASE("Un medidor sube y baja", "[obs][metrics][RF-806]")
{
    Gauge queueDepth;
    queueDepth.set(120);
    REQUIRE(queueDepth.value() == 120);

    queueDepth.add(30);
    queueDepth.sub(50);
    REQUIRE(queueDepth.value() == 100);

    queueDepth.set(0);
    REQUIRE(queueDepth.value() == 0);
}

TEST_CASE("El histograma reparte las muestras en sus cubos fijos", "[obs][metrics][OBS-02]")
{
    Histogram decode;
    decode.observe(500);          // 0,5 us
    decode.observe(30'000);       // 30 us
    decode.observe(30'000);
    decode.observe(2'000'000);    // 2 ms

    REQUIRE(decode.count() == 4);
    REQUIRE(decode.sumNs() == 500 + 30'000 + 30'000 + 2'000'000);
    REQUIRE(decode.minNs() == 500);
    REQUIRE(decode.maxNs() == 2'000'000);
    REQUIRE(decode.bucket(0) == 1);   // <= 1 us
}

TEST_CASE("El histograma sin muestras no inventa cuantiles", "[obs][metrics]")
{
    const Histogram empty;
    REQUIRE(empty.count() == 0);
    REQUIRE(empty.minNs() == 0);
    REQUIRE(empty.maxNs() == 0);
    REQUIRE(empty.quantileNs(0.99) == 0);
}

TEST_CASE("Los cuantiles acotan por arriba el valor real", "[obs][metrics][RNF-002]")
{
    Histogram decode;
    for (int i = 0; i < 99; ++i) { decode.observe(8'000); }   // 8 us
    decode.observe(120'000);                                   // un caso lento

    REQUIRE(decode.count() == 100);
    REQUIRE(decode.quantileNs(0.50) <= 10'000);
    REQUIRE(decode.quantileNs(0.99) <= 250'000);
    REQUIRE(decode.quantileNs(0.99) >= decode.quantileNs(0.50));
}

TEST_CASE("Una muestra mayor que el ultimo limite cae en el desbordamiento", "[obs][metrics]")
{
    Histogram slow;
    slow.observe(5'000'000'000ULL);   // 5 s

    REQUIRE(slow.bucket(Histogram::kBucketCount - 1) == 1);
    REQUIRE(slow.quantileNs(0.99) == 5'000'000'000ULL);
}

TEST_CASE("El temporizador de ambito registra al salir", "[obs][metrics]")
{
    Histogram scoped;
    {
        PW_TIMED(scoped);
        volatile int sink = 0;
        for (int i = 0; i < 1000; ++i) { sink = sink + i; }
    }
    REQUIRE(scoped.count() == 1);
}

TEST_CASE("El registro devuelve siempre la misma instancia para una metrica",
          "[obs][metrics][OBS-01]")
{
    MetricsRegistry registry;

    registry.counter("udp_packets_received_total", "packet_id=6").inc(10);
    registry.counter("udp_packets_received_total", "packet_id=6").inc(5);

    REQUIRE(registry.counter("udp_packets_received_total", "packet_id=6").value() == 15);
    REQUIRE(registry.size() == 1);
}

TEST_CASE("Las etiquetas distinguen metricas del mismo nombre", "[obs][metrics][RF-800]")
{
    MetricsRegistry registry;

    registry.counter("udp_packets_invalid_total", "reason=truncated").inc(3);
    registry.counter("udp_packets_invalid_total", "reason=bad_size").inc(7);

    REQUIRE(registry.size() == 2);
    REQUIRE(registry.counter("udp_packets_invalid_total", "reason=truncated").value() == 3);
    REQUIRE(registry.counter("udp_packets_invalid_total", "reason=bad_size").value() == 7);
}

TEST_CASE("Reutilizar un nombre con otro instrumento es un error de programacion",
          "[obs][metrics]")
{
    MetricsRegistry registry;
    registry.counter("pipeline_queue_depth").inc();

    REQUIRE_THROWS_AS(registry.gauge("pipeline_queue_depth"), std::logic_error);
}

TEST_CASE("La instantanea es consultable y esta ordenada", "[obs][metrics][OBS-01][OBS-03]")
{
    MetricsRegistry registry;
    registry.counter("udp_packets_received_total").inc(415);
    registry.gauge("pipeline_queue_depth", "queue=raw").set(12);
    registry.histogram("decode_duration_ns", "packet_id=6").observe(30'000);

    const auto samples = registry.snapshot();
    REQUIRE(samples.size() == 3);

    REQUIRE(samples[0].name == "decode_duration_ns");
    REQUIRE(samples[0].kind == MetricKind::Histogram);
    REQUIRE(samples[0].count == 1);

    REQUIRE(samples[1].name == "pipeline_queue_depth");
    REQUIRE(samples[1].kind == MetricKind::Gauge);
    REQUIRE(samples[1].gauge == 12);

    REQUIRE(samples[2].name == "udp_packets_received_total");
    REQUIRE(samples[2].kind == MetricKind::Counter);
    REQUIRE(samples[2].count == 415);
}

TEST_CASE("Las referencias del registro siguen siendo validas al crecer", "[obs][metrics]")
{
    MetricsRegistry registry;
    auto&           first = registry.counter("primera");

    for (int i = 0; i < 500; ++i) {
        registry.counter("relleno_" + std::to_string(i)).inc();
    }
    first.inc(42);

    REQUIRE(registry.counter("primera").value() == 42);
}
