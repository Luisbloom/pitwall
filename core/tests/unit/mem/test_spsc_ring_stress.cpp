// Prueba de estres de la cola (F1.1-03, RNF-054).
//
// Dos hilos, 10 millones de elementos, y cuatro propiedades que verificar:
//
//   orden        lo que sale sale en el mismo orden en que entro
//   sin perdida  salen exactamente los 10 millones que entraron
//   sin duplicar ninguno sale dos veces
//   sin corromper cada elemento llega con el contenido con el que entro
//
// Las tres primeras se verifican con un contador esperado: si el valor que sale
// no es el siguiente, o falta, o se repite, la comprobacion de orden ya falla.
// La cuarta necesita carga util: cada elemento lleva una suma de verificacion
// calculada en el productor y comprobada en el consumidor, porque un elemento
// puede llegar en orden y con el payload a medio escribir si la publicacion no
// esta bien ordenada.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include "pw/core/time.hpp"
#include "pw/mem/spsc_ring.hpp"

using pw::mem::SpscRing;

namespace {

/// Elemento con carga util, para que la corrupcion sea detectable. Ocupa 32 B:
/// asi un elemento cruza la frontera de linea de cache cada dos, que es la
/// situacion incomoda de verdad para la publicacion.
struct StressItem {
    std::uint64_t sequence{0};
    std::uint64_t payloadA{0};
    std::uint64_t payloadB{0};
    std::uint64_t checksum{0};

    static constexpr std::uint64_t mix(const std::uint64_t value) noexcept
    {
        // splitmix64: barato y sensible a cualquier bit que cambie.
        std::uint64_t z = value + 0x9E37'79B9'7F4A'7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31);
    }

    static StressItem make(const std::uint64_t sequence) noexcept
    {
        StressItem item;
        item.sequence = sequence;
        item.payloadA = mix(sequence);
        item.payloadB = mix(sequence ^ 0xFFFF'FFFF'FFFF'FFFFULL);
        item.checksum = item.sequence ^ item.payloadA ^ item.payloadB;
        return item;
    }

    [[nodiscard]] bool intact() const noexcept
    {
        return checksum == (sequence ^ payloadA ^ payloadB) && payloadA == mix(sequence) &&
               payloadB == mix(sequence ^ 0xFFFF'FFFF'FFFF'FFFFULL);
    }
};

constexpr std::size_t   kRingCapacity = 8192;   // la de Qraw y Qrec (04-ARCHITECTURE 4.3)
constexpr std::uint64_t kItems        = 10'000'000;

} // namespace

TEST_CASE("Diez millones de elementos entre dos hilos: orden, integridad y cero perdidas",
          "[mem][ring][stress][RNF-054]")
{
    auto ring = std::make_unique<SpscRing<StressItem, kRingCapacity>>();

    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<std::uint64_t> pushRetries{0};
    std::atomic<std::uint64_t> popRetries{0};
    std::atomic<std::uint64_t> outOfOrder{0};
    std::atomic<std::uint64_t> corrupted{0};

    const auto startedNs = pw::monotonicNowNs();

    std::thread producer{[&] {
        std::uint64_t retries = 0;
        for (std::uint64_t i = 0; i < kItems; ++i) {
            const auto item = StressItem::make(i);
            while (!ring->tryPush(item)) {
                ++retries;   // la cola esta llena: el consumidor va por detras
            }
        }
        pushRetries.store(retries, std::memory_order_relaxed);
        produced.store(kItems, std::memory_order_relaxed);
    }};

    std::thread consumer{[&] {
        std::uint64_t expected = 0;
        std::uint64_t retries  = 0;
        std::uint64_t disorder = 0;
        std::uint64_t broken   = 0;

        StressItem item;
        while (expected < kItems) {
            if (!ring->tryPop(item)) {
                ++retries;   // la cola esta vacia: el productor va por detras
                continue;
            }
            // Orden, ausencia de perdidas y ausencia de duplicados, las tres a la
            // vez: el unico valor aceptable es exactamente el siguiente.
            if (item.sequence != expected) { ++disorder; }
            if (!item.intact()) { ++broken; }
            ++expected;
        }

        popRetries.store(retries, std::memory_order_relaxed);
        outOfOrder.store(disorder, std::memory_order_relaxed);
        corrupted.store(broken, std::memory_order_relaxed);
        consumed.store(expected, std::memory_order_relaxed);
    }};

    producer.join();
    consumer.join();

    const auto elapsedNs = pw::monotonicNowNs() - startedNs;

    REQUIRE(produced.load() == kItems);
    REQUIRE(consumed.load() == kItems);
    REQUIRE(outOfOrder.load() == 0);
    REQUIRE(corrupted.load() == 0);
    REQUIRE(ring->pushed() == kItems);
    REQUIRE(ring->popped() == kItems);
    REQUIRE(ring->empty());

    const auto elapsedMs = elapsedNs / 1'000'000;
    const auto perSecond = elapsedNs == 0
                               ? 0
                               : (kItems * 1'000'000'000ULL) / elapsedNs;

    WARN("10 M elementos en " << elapsedMs << " ms  ("
                              << (perSecond / 1'000'000) << " M elementos/s)  "
                              << "reintentos de push: " << pushRetries.load()
                              << ", de pop: " << popRetries.load());
}

TEST_CASE("La cola sostiene el ritmo real del juego con margen de sobra",
          "[mem][ring][stress][RNF-001]")
{
    // El caudal real son 415 datagramas/s, y la prueba de carga de F1.1-14 llega
    // a 1.000/s. Esta comprobacion fija un suelo muy por encima -100.000/s- para
    // que una regresion grave de rendimiento se note aqui y no en pista.
    constexpr std::uint64_t kSampleItems = 1'000'000;

    auto ring = std::make_unique<SpscRing<std::uint64_t, 1024>>();

    const auto startedNs = pw::monotonicNowNs();

    std::thread producer{[&] {
        for (std::uint64_t i = 0; i < kSampleItems; ++i) {
            while (!ring->tryPush(i)) {}
        }
    }};

    std::uint64_t received = 0;
    std::uint64_t value    = 0;
    while (received < kSampleItems) {
        if (ring->tryPop(value)) { ++received; }
    }
    producer.join();

    const auto elapsedNs   = pw::monotonicNowNs() - startedNs;
    const auto itemsPerSec = elapsedNs == 0 ? UINT64_MAX
                                            : (kSampleItems * 1'000'000'000ULL) / elapsedNs;

    REQUIRE(received == kSampleItems);
    REQUIRE(itemsPerSec > 100'000);

    WARN("caudal sostenido: " << (itemsPerSec / 1000) << " K elementos/s");
}
