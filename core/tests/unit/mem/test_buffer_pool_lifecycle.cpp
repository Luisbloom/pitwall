// CICLO DE VIDA DEL BUFFER (F1.1-01): la ventana entre «el buffer muere» y
// «el handle caduca».
//
// Con el protocolo de estado en una sola palabra atomica esa ventana no existe:
// la ultima referencia que se suelta pone refs a 0 y sube la generacion en el
// MISMO compare_exchange, y solo despues devuelve el buffer a la lista de
// libres. Estas pruebas atacan exactamente ese punto.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "pw/mem/buffer_pool.hpp"

using pw::mem::BufferHandle;
using pw::mem::BufferPool;

TEST_CASE("El handle muere con la ultima referencia, no con la siguiente adquisicion",
          "[mem][pool][lifecycle]")
{
    BufferPool pool{4};

    const auto handle = pool.acquire();
    pool.setPayloadSize(handle, 128);
    REQUIRE(pool.alive(handle));
    REQUIRE_FALSE(pool.bytes(handle).empty());

    REQUIRE(pool.release(handle));

    // Aqui NADIE ha vuelto a adquirir la ranura todavia. Aun asi, el handle ya
    // no vale: el buffer esta en la lista de libres y podria entregarse a otro
    // hilo en cualquier instante.
    REQUIRE_FALSE(pool.alive(handle));
    REQUIRE(pool.bytes(handle).empty());
    REQUIRE(pool.payload(handle).empty());
    REQUIRE(pool.payloadSize(handle) == 0);
    REQUIRE(pool.refCount(handle) == 0);
    REQUIRE(pool.stats().stale > 0);

    // Y la ranura sigue perfectamente utilizable, con la generacion siguiente.
    const auto reused = pool.acquire();
    REQUIRE(reused.index() == handle.index());
    REQUIRE(reused.generation() == handle.generation() + 1);
    REQUIRE(pool.alive(reused));
    REQUIRE(pool.release(reused));
}

TEST_CASE("retain() sobre un handle ya soltado no resucita el buffer",
          "[mem][pool][lifecycle]")
{
    BufferPool pool{2};

    const auto handle = pool.acquire();
    REQUIRE(pool.release(handle));
    REQUIRE(pool.inUse() == 0);

    // Si esto resucitara el buffer, la ranura tendria dos duenos a la vez: la
    // lista de libres y quien acaba de incrementar.
    REQUIRE_FALSE(pool.retain(handle));
    REQUIRE(pool.stats().rejectedRetains == 1);
    REQUIRE(pool.refCount(handle) == 0);
    REQUIRE(pool.inUse() == 0);
    REQUIRE(pool.available() == 2);

    // El pool sigue entero: entrega sus dos buffers y ni uno mas.
    const auto a = pool.acquire();
    const auto b = pool.acquire();
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    REQUIRE(a.index() != b.index());
    REQUIRE_FALSE(pool.acquire().valid());

    REQUIRE(pool.release(a));
    REQUIRE(pool.release(b));
}

TEST_CASE("Un release duplicado se rechaza y no hace underflow",
          "[mem][pool][lifecycle]")
{
    BufferPool pool{2};

    const auto handle = pool.acquire();
    REQUIRE(pool.release(handle));         // la buena
    REQUIRE_FALSE(pool.release(handle));   // la de mas
    REQUIRE_FALSE(pool.release(handle));   // y otra

    REQUIRE(pool.stats().rejectedReleases == 2);
    REQUIRE(pool.stats().released == 1);   // una sola devolucion real
    REQUIRE(pool.inUse() == 0);
    REQUIRE(pool.available() == 2);

    // Si refs hubiera bajado de 0, el contador habria dado la vuelta a 2^32-1 y
    // la ranura no volveria a morir nunca. Se comprueba que el pool entrega
    // exactamente sus dos buffers, y distintos.
    const auto a = pool.acquire();
    const auto b = pool.acquire();
    REQUIRE(a.index() != b.index());
    REQUIRE_FALSE(pool.acquire().valid());
    REQUIRE(pool.release(a));
    REQUIRE(pool.release(b));
    REQUIRE(pool.available() == 2);
}

TEST_CASE("Las referencias intermedias no matan el buffer", "[mem][pool][lifecycle]")
{
    BufferPool pool{2};

    const auto handle = pool.acquire();
    REQUIRE(pool.retain(handle));
    REQUIRE(pool.retain(handle));
    REQUIRE(pool.refCount(handle) == 3);

    REQUIRE(pool.release(handle));
    REQUIRE(pool.alive(handle));             // sigue vivo con 2
    REQUIRE_FALSE(pool.bytes(handle).empty());

    REQUIRE(pool.release(handle));
    REQUIRE(pool.alive(handle));             // sigue vivo con 1
    REQUIRE(pool.refCount(handle) == 1);

    REQUIRE(pool.release(handle));
    REQUIRE_FALSE(pool.alive(handle));       // y ahora si
    REQUIRE(pool.bytes(handle).empty());
    REQUIRE(pool.stats().released == 1);
}

TEST_CASE("retain y release concurrentes alrededor de la ultima referencia",
          "[mem][pool][lifecycle][concurrency]")
{
    // La carrera que el protocolo tiene que ganar: mientras un hilo suelta la
    // ultima referencia, otro intenta tomar una.
    //
    // El retenedor no lo intenta una vez: MARTILLEA la ventana con retain +
    // release hasta que el buffer muere. Asi se recorren de verdad las dos
    // ramas -la retencion que llega a tiempo y la que llega tarde- en lugar de
    // depender de que dos hilos arranquen a la vez.
    //
    // Invariantes: si un retain gana, el buffer sigue vivo; cuando muere, no
    // queda ninguna referencia; y ninguna ranura se devuelve dos veces al pool.
    constexpr int kRounds = 5'000;

    BufferPool pool{8};

    std::uint64_t retainsWon    = 0;
    std::uint64_t retainsLost   = 0;
    std::uint64_t roundsWithWin = 0;

    std::atomic<bool> deadWhileRetained{false};
    std::atomic<bool> survived{false};

    for (int round = 0; round < kRounds; ++round) {
        const auto handle = pool.acquire();
        if (!handle.valid()) { FAIL("el pool deberia tener buffers libres"); }

        std::atomic<bool>          go{false};
        std::atomic<std::uint64_t> won{0};
        std::atomic<std::uint64_t> lost{0};

        std::thread releaser{[&] {
            while (!go.load(std::memory_order_acquire)) {}
            pool.release(handle);   // la ultima referencia
        }};

        std::thread retainer{[&] {
            while (!go.load(std::memory_order_acquire)) {}
            for (;;) {
                if (pool.retain(handle)) {
                    won.fetch_add(1, std::memory_order_relaxed);
                    // Si el retain gano, el buffer TIENE que seguir vivo.
                    if (!pool.alive(handle)) { deadWhileRetained.store(true); }
                    pool.release(handle);
                } else {
                    lost.fetch_add(1, std::memory_order_relaxed);
                    break;   // el buffer ya ha muerto: no se puede resucitar
                }
            }
        }};

        go.store(true, std::memory_order_release);
        releaser.join();
        retainer.join();

        const auto roundWon = won.load(std::memory_order_relaxed);
        retainsWon += roundWon;
        retainsLost += lost.load(std::memory_order_relaxed);
        if (roundWon > 0) { ++roundsWithWin; }

        if (pool.alive(handle)) { survived.store(true); }
    }

    REQUIRE_FALSE(deadWhileRetained.load());
    REQUIRE_FALSE(survived.load());
    REQUIRE(pool.inUse() == 0);

    // Las dos ramas se han recorrido: cada ronda acaba con un rechazo, y en
    // muchas hubo antes al menos una retencion ganada.
    REQUIRE(retainsLost == kRounds);
    REQUIRE(roundsWithWin > 0);

    // La comprobacion decisiva de que ninguna ranura se devolvio dos veces a la
    // lista de libres: el pool entrega EXACTAMENTE su capacidad, con indices
    // distintos. Una doble devolucion daria indices repetidos, o de mas.
    std::vector<BufferHandle> all;
    for (;;) {
        const auto handle = pool.acquire();
        if (!handle.valid()) { break; }
        all.push_back(handle);
    }
    REQUIRE(all.size() == pool.capacity());

    std::vector<std::uint32_t> indices;
    indices.reserve(all.size());
    for (const auto handle : all) { indices.push_back(handle.index()); }
    std::sort(indices.begin(), indices.end());
    REQUIRE(std::adjacent_find(indices.begin(), indices.end()) == indices.end());

    for (const auto handle : all) { REQUIRE(pool.release(handle)); }

    WARN("carrera retain/release en " << kRounds << " rondas: " << retainsWon
                                      << " retenciones ganadas en " << roundsWithWin
                                      << " rondas, " << retainsLost << " rechazadas");
}

TEST_CASE("La misma ranura se reutiliza cien mil veces sin confundir generaciones",
          "[mem][pool][lifecycle]")
{
    // Un pool de uno fuerza a que todas las vidas ocurran sobre la MISMA ranura.
    BufferPool pool{1, 128};

    BufferHandle previous;
    bool         generationsIncrease = true;
    bool         oldHandlesDie       = true;
    bool         acquireFailed       = false;

    for (int i = 0; i < 100'000; ++i) {
        const auto handle = pool.acquire();
        if (!handle.valid()) {
            acquireFailed = true;
            break;
        }

        if (previous.valid()) {
            if (handle.generation() != previous.generation() + 1) { generationsIncrease = false; }
            if (pool.alive(previous)) { oldHandlesDie = false; }
            if (!pool.bytes(previous).empty()) { oldHandlesDie = false; }
        }

        pool.setPayloadSize(handle, 64);
        previous = handle;
        pool.release(handle);
    }

    REQUIRE_FALSE(acquireFailed);
    REQUIRE(generationsIncrease);
    REQUIRE(oldHandlesDie);
    REQUIRE(pool.stats().acquired == 100'000);
    REQUIRE(pool.stats().released == 100'000);
    REQUIRE(pool.inUse() == 0);
}

TEST_CASE("El recuento de referencias se detiene en su maximo",
          "[mem][pool][lifecycle]")
{
    // refs son los 16 bits bajos del estado: incrementar en el maximo acarrearia
    // hacia la generacion y cambiaria la identidad del buffer sin que nadie lo
    // hubiera pedido. Es el unico camino por el que la generacion podria moverse
    // sin una muerte de por medio.
    //
    // Se llega al tope con la API publica, sin exponer nada solo para probar:
    // 65.535 retenciones son instantaneas.
    BufferPool pool{2};

    const auto handle = pool.acquire();          // refs = 1
    const auto generationBefore = pool.generationOf(handle.index());

    bool allAccepted = true;
    for (std::uint32_t i = 1; i < pw::mem::kMaxBufferRefs; ++i) {
        if (!pool.retain(handle)) { allAccepted = false; break; }
    }
    REQUIRE(allAccepted);
    REQUIRE(pool.refCount(handle) == pw::mem::kMaxBufferRefs);

    // El que sobra se rechaza, y la identidad del buffer no se ha movido.
    REQUIRE_FALSE(pool.retain(handle));
    REQUIRE_FALSE(pool.retain(handle));
    REQUIRE(pool.stats().rejectedRetains == 2);
    REQUIRE(pool.refCount(handle) == pw::mem::kMaxBufferRefs);
    REQUIRE(pool.generationOf(handle.index()) == generationBefore);
    REQUIRE(pool.alive(handle));

    // Y se deshace entero sin dejar rastro.
    for (std::uint32_t i = 0; i < pw::mem::kMaxBufferRefs; ++i) {
        if (!pool.release(handle)) { FAIL("release deberia aceptarse mientras queden referencias"); }
    }
    REQUIRE_FALSE(pool.alive(handle));
    REQUIRE(pool.inUse() == 0);
    REQUIRE(pool.stats().released == 1);
}

TEST_CASE("La generacion avanza de una en una y nunca vale cero",
          "[mem][pool][lifecycle]")
{
    BufferPool pool{1};

    // La generacion 0 esta reservada para «ninguna»: es la de un handle por
    // defecto, y ninguna ranura puede tenerla ni al nacer ni tras reciclarse.
    REQUIRE(pool.generationOf(0) != 0);

    std::uint64_t previous = pool.generationOf(0);
    for (int i = 0; i < 1000; ++i) {
        const auto handle = pool.acquire();
        REQUIRE(handle.generation() == previous);
        REQUIRE(handle.generation() != 0);
        REQUIRE(pool.release(handle));

        const auto current = pool.generationOf(0);
        REQUIRE(current == previous + 1);
        REQUIRE(current != 0);
        previous = current;
    }
}

TEST_CASE("La generacion de un handle tiene 48 bits utiles", "[mem][pool][lifecycle]")
{
    // Si el handle solo llevara 32 bits de generacion, ampliar la ranura a 48 no
    // serviria de nada: lo que tiene que ser distinguible entre dos vidas es el
    // handle. Esta comprobacion fija esa propiedad en el tipo.
    constexpr std::uint64_t highGeneration = 0x0000'FFFF'FFFF'FFFFULL;
    const BufferHandle      handle{0, highGeneration};

    REQUIRE(handle.generation() == highGeneration);
    REQUIRE(handle.generation() > 0xFFFF'FFFFULL);
}

TEST_CASE("Un handle inventado no da acceso a nada", "[mem][pool][lifecycle]")
{
    BufferPool pool{4};

    const BufferHandle nunca;                    // por defecto: invalido
    const BufferHandle fueraDeRango{99, 1};
    const BufferHandle generacionFalsa{0, 12345};

    for (const auto handle : {nunca, fueraDeRango, generacionFalsa}) {
        REQUIRE_FALSE(pool.alive(handle));
        REQUIRE(pool.bytes(handle).empty());
        REQUIRE(pool.refCount(handle) == 0);
        REQUIRE_FALSE(pool.retain(handle));
        REQUIRE_FALSE(pool.release(handle));
    }

    REQUIRE(pool.inUse() == 0);
    REQUIRE(pool.available() == 4);
}
