// Pruebas de SpscRing (F1.1-02): bordes, semantica de capacidad y envolvente.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <type_traits>

#include "pw/mem/buffer_pool.hpp"
#include "pw/mem/spsc_ring.hpp"

using pw::mem::BufferHandle;
using pw::mem::SpscRing;

TEST_CASE("Una cola de N almacena exactamente N elementos", "[mem][ring][capacity]")
{
    // La semantica es inequivoca y esta documentada en la cabecera: N, no N-1.
    // Los indices son contadores monotonos, asi que no hace falta sacrificar un
    // hueco para distinguir «llena» de «vacia».
    SpscRing<int, 4> ring;

    REQUIRE(SpscRing<int, 4>::capacity() == 4);
    REQUIRE(ring.empty());

    for (int i = 0; i < 4; ++i) {
        INFO("empujando el elemento " << i);
        REQUIRE(ring.tryPush(i));
    }

    REQUIRE(ring.size() == 4);
    REQUIRE(ring.full());
    REQUIRE_FALSE(ring.tryPush(99));   // el quinto no cabe
    REQUIRE(ring.size() == 4);

    for (int i = 0; i < 4; ++i) {
        int value = -1;
        REQUIRE(ring.tryPop(value));
        REQUIRE(value == i);
    }
    REQUIRE(ring.empty());
}

TEST_CASE("Una cola vacia no entrega nada", "[mem][ring][edge]")
{
    SpscRing<int, 8> ring;

    int value = 42;
    REQUIRE_FALSE(ring.tryPop(value));
    REQUIRE(value == 42);              // tryPop fallido no toca el destino
    REQUIRE(ring.size() == 0);
    REQUIRE(ring.empty());
    REQUIRE_FALSE(ring.full());
}

TEST_CASE("Un solo elemento entra y sale", "[mem][ring][edge]")
{
    SpscRing<int, 2> ring;

    REQUIRE(ring.tryPush(7));
    REQUIRE(ring.size() == 1);
    REQUIRE_FALSE(ring.empty());
    REQUIRE_FALSE(ring.full());

    int value = 0;
    REQUIRE(ring.tryPop(value));
    REQUIRE(value == 7);
    REQUIRE(ring.empty());
    REQUIRE_FALSE(ring.tryPop(value));
}

TEST_CASE("Una cola llena rechaza sin perder lo que ya tenia", "[mem][ring][edge]")
{
    SpscRing<int, 2> ring;

    REQUIRE(ring.tryPush(1));
    REQUIRE(ring.tryPush(2));
    REQUIRE(ring.full());

    REQUIRE_FALSE(ring.tryPush(3));
    REQUIRE_FALSE(ring.tryPush(4));    // rechazar no corrompe nada

    int value = 0;
    REQUIRE(ring.tryPop(value));
    REQUIRE(value == 1);
    REQUIRE(ring.tryPush(3));          // al liberarse un hueco, vuelve a admitir
    REQUIRE(ring.tryPop(value));
    REQUIRE(value == 2);
    REQUIRE(ring.tryPop(value));
    REQUIRE(value == 3);
    REQUIRE(ring.empty());
}

TEST_CASE("Los indices dan la vuelta sin alterar el orden", "[mem][ring][wrap]")
{
    // Con capacidad 4 y 1.000 elementos, los indices envuelven 250 veces.
    SpscRing<int, 4> ring;

    int  expected = 0;
    bool ordered  = true;

    for (int i = 0; i < 1000; ++i) {
        if (!ring.tryPush(i)) { FAIL("la cola deberia admitir el elemento " << i); }
        if ((i % 3) == 2) {
            for (int k = 0; k < 3; ++k) {
                int value = -1;
                if (!ring.tryPop(value)) { FAIL("la cola deberia entregar un elemento"); }
                if (value != expected++) { ordered = false; }
            }
        }
    }

    int value = -1;
    while (ring.tryPop(value)) {
        if (value != expected++) { ordered = false; }
    }

    REQUIRE(ordered);
    REQUIRE(expected == 1000);
    REQUIRE(ring.empty());
}

TEST_CASE("Llenar y vaciar del todo, muchas veces, no degrada la cola",
          "[mem][ring][wrap]")
{
    // Reutilizacion prolongada: 100.000 ciclos de llenado y vaciado completos.
    constexpr std::size_t kCapacity = 8;
    SpscRing<std::uint64_t, kCapacity> ring;

    bool ok = true;
    for (std::uint64_t cycle = 0; cycle < 100'000 && ok; ++cycle) {
        for (std::uint64_t i = 0; i < kCapacity; ++i) {
            if (!ring.tryPush(cycle * kCapacity + i)) { ok = false; }
        }
        if (ring.tryPush(0)) { ok = false; }   // esta llena de verdad

        for (std::uint64_t i = 0; i < kCapacity; ++i) {
            std::uint64_t value = 0;
            if (!ring.tryPop(value)) { ok = false; }
            if (value != cycle * kCapacity + i) { ok = false; }
        }
        std::uint64_t drained = 0;
        if (ring.tryPop(drained)) { ok = false; }   // y vacia de verdad
    }

    REQUIRE(ok);
    REQUIRE(ring.empty());
    REQUIRE(ring.pushed() == 100'000 * kCapacity);
    REQUIRE(ring.popped() == 100'000 * kCapacity);
}

TEST_CASE("La cola transporta handles del pool, no punteros", "[mem][ring]")
{
    // Es la combinacion real: Qraw y Qrec llevan BufferHandle.
    auto ring = std::make_unique<SpscRing<BufferHandle, 16>>();

    const BufferHandle handle{7, 3};
    REQUIRE(ring->tryPush(handle));

    BufferHandle out;
    REQUIRE(ring->tryPop(out));
    REQUIRE(out == handle);
    REQUIRE(out.index() == 7);
    REQUIRE(out.generation() == 3);
}

TEST_CASE("La cola no comparte linea de cache entre productor y consumidor",
          "[mem][ring][RNF-051]")
{
    // La verificacion fuerte son los static_assert de la cabecera, que fallan en
    // compilacion. Esto documenta la consecuencia observable: los dos bloques de
    // indices ocupan al menos dos lineas de cache completas.
    STATIC_REQUIRE(alignof(SpscRing<int, 4>) >= pw::mem::kCacheLineBytes);
    STATIC_REQUIRE(sizeof(SpscRing<int, 4>) >= 2 * pw::mem::kCacheLineBytes);
}

TEST_CASE("El tamano es orientativo y los contadores historicos exactos",
          "[mem][ring]")
{
    SpscRing<int, 4> ring;

    REQUIRE(ring.pushed() == 0);
    REQUIRE(ring.popped() == 0);

    REQUIRE(ring.tryPush(1));
    REQUIRE(ring.tryPush(2));
    int value = 0;
    REQUIRE(ring.tryPop(value));

    REQUIRE(ring.pushed() == 2);
    REQUIRE(ring.popped() == 1);
    REQUIRE(ring.size() == 1);
}
