// Pruebas de BufferPool (F1.1-01, RNF-013).
//
// El foco es el MODELO DE PROPIEDAD: un mismo datagrama alimenta al pipeline y
// al grabador sin copiarse, y vuelve al pool cuando lo suelta el ultimo.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "pw/mem/buffer_pool.hpp"

using pw::mem::BufferHandle;
using pw::mem::BufferPool;

namespace {

/// Escribe un patron reconocible en el buffer, como haria recvfrom.
void fill(BufferPool& pool, const BufferHandle handle, const std::byte value,
          const std::size_t size)
{
    const auto bytes = pool.bytes(handle);
    REQUIRE(bytes.size() >= size);
    std::fill_n(bytes.begin(), size, value);
    pool.setPayloadSize(handle, size);
}

} // namespace

TEST_CASE("El pool reserva toda su memoria al construirse", "[mem][pool][RNF-013]")
{
    const BufferPool pool{16, 2048};

    REQUIRE(pool.capacity() == 16);
    REQUIRE(pool.bufferBytes() == 2048);
    REQUIRE(pool.available() == 16);
    REQUIRE(pool.inUse() == 0);
    REQUIRE(pool.reservedBytes() >= 16 * 2048);
}

TEST_CASE("El tamano de buffer se redondea a linea de cache", "[mem][pool]")
{
    // Dos buffers en vuelo en hilos distintos no deben compartir linea de cache.
    const BufferPool pool{4, 1470};
    REQUIRE(pool.bufferBytes() == 1472);   // 1470 -> siguiente multiplo de 64
    REQUIRE(pool.bufferBytes() % 64 == 0);
}

TEST_CASE("Adquirir y soltar devuelve el buffer al pool", "[mem][pool]")
{
    BufferPool pool{4};

    const auto handle = pool.acquire();
    REQUIRE(handle.valid());
    REQUIRE(pool.alive(handle));
    REQUIRE(pool.refCount(handle) == 1);
    REQUIRE(pool.inUse() == 1);
    REQUIRE(pool.available() == 3);

    pool.release(handle);

    REQUIRE(pool.inUse() == 0);
    REQUIRE(pool.available() == 4);
    REQUIRE(pool.stats().released == 1);
}

TEST_CASE("Un pool agotado lo dice y lo cuenta", "[mem][pool][RNF-013]")
{
    BufferPool pool{3};

    std::vector<BufferHandle> held;
    for (int i = 0; i < 3; ++i) {
        const auto handle = pool.acquire();
        REQUIRE(handle.valid());
        held.push_back(handle);
    }

    REQUIRE(pool.available() == 0);

    const auto denied = pool.acquire();
    REQUIRE_FALSE(denied.valid());
    REQUIRE(pool.stats().exhausted == 1);

    // Agotarse no es un fallo: es una degradacion contabilizada. En cuanto se
    // libera uno, el pool vuelve a servir.
    pool.release(held.back());
    held.pop_back();

    const auto recovered = pool.acquire();
    REQUIRE(recovered.valid());
    REQUIRE(pool.stats().exhausted == 1);

    pool.release(recovered);
    for (const auto handle : held) { pool.release(handle); }
    REQUIRE(pool.inUse() == 0);
}

TEST_CASE("El buffer vuelve al pool solo cuando lo suelta el ultimo",
          "[mem][pool][ownership]")
{
    // El escenario real: T1 lo adquiere, lo encola para T2 (pipeline) y para T3
    // (grabador), y suelta su propia referencia.
    BufferPool pool{2};

    const auto handle = pool.acquire();          // refs = 1  (productor)
    REQUIRE(pool.refCount(handle) == 1);

    pool.retain(handle);                          // refs = 2  (Qraw)
    pool.retain(handle);                          // refs = 3  (Qrec)
    REQUIRE(pool.refCount(handle) == 3);

    pool.release(handle);                         // T1 suelta la suya -> 2
    REQUIRE(pool.refCount(handle) == 2);
    REQUIRE(pool.inUse() == 1);

    pool.release(handle);                         // el pipeline termina -> 1
    REQUIRE(pool.refCount(handle) == 1);
    REQUIRE(pool.inUse() == 1);
    REQUIRE(pool.available() == 1);                // sigue fuera del pool

    pool.release(handle);                         // el grabador termina -> 0
    REQUIRE(pool.inUse() == 0);
    REQUIRE(pool.available() == 2);
    // El handle sobrevive al buffer: sigue siendo valido como valor, pero ya no
    // da acceso a nada, que es justo lo que se busca.
    REQUIRE(pool.refCount(handle) == 0);
}

TEST_CASE("Los dos consumidores leen el mismo payload sin copiarlo",
          "[mem][pool][ownership][RNF-013]")
{
    BufferPool pool{4};

    const auto handle = pool.acquire();
    fill(pool, handle, std::byte{0xAB}, 1470);
    pool.retain(handle);   // Qraw
    pool.retain(handle);   // Qrec
    pool.release(handle);  // el productor suelta la suya

    // Lo que ve el pipeline y lo que ve el grabador es LA MISMA memoria.
    const auto asPipeline = pool.payload(handle);
    const auto asRecorder = pool.payload(handle);

    REQUIRE(asPipeline.size() == 1470);
    REQUIRE(asRecorder.size() == 1470);
    REQUIRE(asPipeline.data() == asRecorder.data());
    REQUIRE(asPipeline[0] == std::byte{0xAB});
    REQUIRE(asPipeline[1469] == std::byte{0xAB});

    pool.release(handle);
    pool.release(handle);
    REQUIRE(pool.inUse() == 0);
}

TEST_CASE("Un handle caducado no lee el datagrama de otro", "[mem][pool][ownership]")
{
    // Es la razon de que el handle sea indice + generacion y no un puntero: un
    // uso despues de soltar se detecta en vez de leer datos que parecen validos.
    BufferPool pool{1};

    const auto first = pool.acquire();
    fill(pool, first, std::byte{0x11}, 64);
    pool.release(first);

    const auto second = pool.acquire();   // reutiliza la misma ranura
    fill(pool, second, std::byte{0x22}, 64);

    REQUIRE(second.index() == first.index());
    REQUIRE(second.generation() != first.generation());

    REQUIRE(pool.alive(second));
    REQUIRE_FALSE(pool.alive(first));
    REQUIRE(pool.bytes(first).empty());
    REQUIRE(pool.payload(first).empty());
    REQUIRE(pool.refCount(first) == 0);
    REQUIRE(pool.stats().stale > 0);

    // Y soltar dos veces por error no corrompe el pool.
    pool.release(first);
    REQUIRE(pool.inUse() == 1);

    pool.release(second);
    REQUIRE(pool.inUse() == 0);
}

TEST_CASE("El payload se recorta al tamano real del datagrama", "[mem][pool]")
{
    BufferPool pool{2, 2048};

    const auto handle = pool.acquire();
    REQUIRE(pool.payloadSize(handle) == 0);   // un buffer recien tomado esta vacio

    pool.setPayloadSize(handle, 1349);
    REQUIRE(pool.payloadSize(handle) == 1349);
    REQUIRE(pool.payload(handle).size() == 1349);
    REQUIRE(pool.bytes(handle).size() == 2048);

    // Un tamano imposible se acota en vez de desbordar.
    pool.setPayloadSize(handle, 99999);
    REQUIRE(pool.payloadSize(handle) == 2048);

    pool.release(handle);
}

TEST_CASE("Los buffers salen siempre de la memoria reservada al construir",
          "[mem][pool][RNF-013]")
{
    // Verificacion de «cero asignaciones en ejecucion» al alcance de esta fase:
    // ningun acquire() entrega memoria fuera del bloque reservado, ni siquiera
    // tras un millon de ciclos. La cuenta instrumentada de asignaciones del
    // camino de recepcion corresponde a F1.1-07.
    BufferPool pool{8, 256};

    const auto probe = pool.acquire();
    const auto* base = pool.bytes(probe).data();
    pool.release(probe);

    const auto* const low  = base - (static_cast<std::ptrdiff_t>(probe.index()) * 256);
    const auto* const high = low + (8 * 256);

    // Sin REQUIRE dentro del bucle: un millon de aserciones de Catch2 costarian
    // mas que la propia prueba. Se acumula y se comprueba una vez.
    bool outside = false;
    bool invalid = false;
    for (int i = 0; i < 1'000'000; ++i) {
        const auto handle = pool.acquire();
        if (!handle.valid()) {
            invalid = true;
            break;
        }
        const auto* data = pool.bytes(handle).data();
        if (data < low || data >= high) { outside = true; }
        pool.release(handle);
    }
    REQUIRE_FALSE(invalid);
    REQUIRE_FALSE(outside);

    REQUIRE(pool.inUse() == 0);
    REQUIRE(pool.stats().acquired == 1'000'001);
    REQUIRE(pool.stats().exhausted == 0);
}

TEST_CASE("El recuento de referencias aguanta varios hilos soltando a la vez",
          "[mem][pool][ownership][concurrency]")
{
    // Un productor adquiere y reparte; dos consumidores sueltan. Es el reparto
    // real de T1, T2 y T3, sin las colas todavia.
    constexpr int kIterations = 20'000;

    BufferPool pool{64};

    // El traspaso no empaqueta el handle en un entero: con generaciones de 48
    // bits ya no cabria, y hacerlo a mano es la clase de truco que envejece mal.
    // Se publica el handle y despues una bandera, con release/acquire.
    struct Handoff {
        BufferHandle      handle{};
        std::atomic<bool> ready{false};
    };

    std::vector<Handoff> forPipeline(kIterations);
    std::vector<Handoff> forRecorder(kIterations);

    std::atomic<bool> failed{false};

    std::thread producer{[&] {
        for (int i = 0; i < kIterations; ++i) {
            BufferHandle handle;
            while (!(handle = pool.acquire()).valid()) { std::this_thread::yield(); }

            pool.setPayloadSize(handle, 100);
            if (!pool.retain(handle)) { failed.store(true); }   // para el pipeline
            if (!pool.retain(handle)) { failed.store(true); }   // para el grabador

            const auto slot = static_cast<std::size_t>(i);
            forPipeline[slot].handle = handle;
            forRecorder[slot].handle = handle;
            forPipeline[slot].ready.store(true, std::memory_order_release);
            forRecorder[slot].ready.store(true, std::memory_order_release);

            pool.release(handle);   // el productor suelta la suya
        }
    }};

    const auto consume = [&](std::vector<Handoff>& slots) {
        for (int i = 0; i < kIterations; ++i) {
            auto& slot = slots[static_cast<std::size_t>(i)];
            while (!slot.ready.load(std::memory_order_acquire)) { std::this_thread::yield(); }

            const auto handle = slot.handle;
            if (!pool.alive(handle)) { failed.store(true); }
            if (pool.payloadSize(handle) != 100) { failed.store(true); }
            if (!pool.release(handle)) { failed.store(true); }
        }
    };

    std::thread consumerA{[&] { consume(forPipeline); }};
    std::thread consumerB{[&] { consume(forRecorder); }};

    producer.join();
    consumerA.join();
    consumerB.join();

    REQUIRE_FALSE(failed.load());
    REQUIRE(pool.inUse() == 0);
    REQUIRE(pool.available() == 64);
    REQUIRE(pool.stats().acquired == kIterations);
    REQUIRE(pool.stats().released == kIterations);
    REQUIRE(pool.stats().rejectedRetains == 0);
    REQUIRE(pool.stats().rejectedReleases == 0);
}

TEST_CASE("El pool rechaza configuraciones imposibles", "[mem][pool]")
{
    REQUIRE_THROWS_AS(BufferPool(0, 2048), std::invalid_argument);
    REQUIRE_THROWS_AS(BufferPool(8, 0), std::invalid_argument);
}
