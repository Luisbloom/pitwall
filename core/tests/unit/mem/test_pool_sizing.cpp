// VERIFICACION DEL DIMENSIONADO DEL POOL (F1.1-01).
//
// Esta prueba existe porque la documentacion afirmaba -en dos versiones
// seguidas, con dos numeros distintos- que el pool estaba bien dimensionado, y
// las dos veces se quedaba corto. No razona sobre el tamano: reproduce el
// reparto real -un datagrama, dos colas, dos consumidores, recuento de
// referencias- y mide cuando se agota el pool.
//
// El resultado, en una frase: el pool no es el cuello de botella solo si SUPERA
// LA SUMA de las capacidades de las colas, porque cada cola mantiene su propia
// referencia y dos colas que avanzan a ritmos distintos acaban refiriendose a
// conjuntos de buffers disjuntos.
//
// A escala reducida (colas de 32) es exactamente el mismo fenomeno que a escala
// real (colas de 8192), y se ejecuta en milisegundos.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

#include "pw/mem/buffer_pool.hpp"
#include "pw/mem/spsc_ring.hpp"

using pw::mem::BufferHandle;
using pw::mem::BufferPool;
using pw::mem::poolSizeForQueues;
using pw::mem::SpscRing;

namespace {

constexpr std::size_t kQueueCapacity = 32;

using Queue = SpscRing<BufferHandle, kQueueCapacity>;

struct Outcome {
    std::uint64_t delivered{0};       ///< datagramas que llegaron al pipeline
    std::uint64_t droppedByPool{0};   ///< perdidos porque no habia buffer
    std::uint64_t droppedByQrec{0};   ///< perdidos por la politica de Qrec
    std::size_t   qrawDepth{0};
    std::size_t   qrecDepth{0};
};

/// Reproduce el reparto de T1 con el grabador COMPLETAMENTE ATASCADO: Qrec no
/// se drena nunca. Es el caso que decide el dimensionado, y no es hipotetico:
/// un disco lento o una rotacion de fichero bastan para producirlo.
Outcome runWithStalledRecorder(BufferPool& pool, const std::uint64_t datagrams)
{
    auto qraw = std::make_unique<Queue>();
    auto qrec = std::make_unique<Queue>();

    Outcome outcome;

    for (std::uint64_t i = 0; i < datagrams; ++i) {
        const auto handle = pool.acquire();
        if (!handle.valid()) {
            ++outcome.droppedByPool;   // el pool se agoto: se pierde el datagrama
            continue;
        }
        pool.setPayloadSize(handle, 1470);

        pool.retain(handle);                       // referencia de Qraw
        if (!qraw->tryPush(handle)) { pool.release(handle); }

        pool.retain(handle);                       // referencia de Qrec
        if (!qrec->tryPush(handle)) {
            pool.release(handle);
            ++outcome.droppedByQrec;               // politica documentada de Qrec
        }

        pool.release(handle);                      // el productor suelta la suya

        // El pipeline (T2) va al dia: consume y suelta en cuanto puede.
        BufferHandle consumed;
        while (qraw->tryPop(consumed)) {
            ++outcome.delivered;
            pool.release(consumed);
        }
        // T3 no consume nada: esta atascado.
    }

    outcome.qrawDepth = qraw->size();
    outcome.qrecDepth = qrec->size();

    // Limpieza: se vacia Qrec para no dejar referencias colgando.
    BufferHandle pending;
    while (qrec->tryPop(pending)) { pool.release(pending); }
    while (qraw->tryPop(pending)) { pool.release(pending); }

    return outcome;
}

} // namespace

TEST_CASE("Un pool del tamano de una cola se agota antes de que la cola se llene",
          "[mem][pool][sizing][RNF-013]")
{
    // El dimensionado que la documentacion daba por bueno, a escala: pool igual
    // a la capacidad de una cola.
    BufferPool pool{kQueueCapacity};

    const auto outcome = runWithStalledRecorder(pool, 500);

    // El grabador atascado retiene los 32 buffers de Qrec. Como el pool solo
    // tiene 32, no queda ninguno para el pipeline: SE PIERDE TODO, tambien el
    // directo, y la politica de desbordamiento de Qrec no llega a aplicarse.
    REQUIRE(outcome.qrecDepth == kQueueCapacity);
    REQUIRE(outcome.droppedByPool > 0);
    REQUIRE(outcome.droppedByQrec == 0);
    REQUIRE(pool.stats().exhausted > 0);

    // El pipeline deja de recibir: el atasco del grabador se ha contagiado.
    REQUIRE(outcome.delivered <= kQueueCapacity);

    WARN("pool = capacidad de una cola (" << kQueueCapacity << "): entregados "
         << outcome.delivered << " de 500, perdidos por pool agotado "
         << outcome.droppedByPool);
}

TEST_CASE("Un pool dimensionado por la suma de las colas deja el atasco en su cola",
          "[mem][pool][sizing][RNF-013]")
{
    // El dimensionado que se deduce del modelo de propiedad: la suma de las
    // capacidades, mas lo que cada consumidor tiene en la mano, mas el que el
    // productor esta rellenando.
    const auto sized = poolSizeForQueues(kQueueCapacity * 2, /*consumerCount=*/2,
                                         /*producerInFlight=*/1);
    REQUIRE(sized == (2 * kQueueCapacity) + 3);

    BufferPool pool{sized};

    const auto outcome = runWithStalledRecorder(pool, 500);

    // Ahora si: Qrec se llena y empieza a rechazar -su politica documentada,
    // contabilizada- mientras el pipeline sigue recibiendo TODOS los datagramas.
    REQUIRE(outcome.qrecDepth == kQueueCapacity);
    REQUIRE(outcome.droppedByPool == 0);
    REQUIRE(outcome.droppedByQrec > 0);
    REQUIRE(outcome.delivered == 500);
    REQUIRE(pool.stats().exhausted == 0);

    WARN("pool = " << sized << ": entregados " << outcome.delivered
                   << " de 500, perdidos por la politica de Qrec "
                   << outcome.droppedByQrec);
}

TEST_CASE("La regla de dimensionado es la suma de las colas, no la mayor de ellas",
          "[mem][pool][sizing]")
{
    // Dos colas de 8192 y dos consumidores, que es la configuracion real de
    // 04-ARCHITECTURE 4.3.
    constexpr std::size_t kQraw = pw::mem::kQrawCapacity;
    constexpr std::size_t kQrec = pw::mem::kQrecCapacity;

    STATIC_REQUIRE(poolSizeForQueues(kQraw + kQrec, 2, 1) == 16387);

    // Lo que interesa dejar escrito para quien decida el valor de configuracion:
    // el minimo por rafaga (415 x 10 = 4150) y el minimo por propiedad (16387)
    // son dos restricciones distintas, y manda la mayor.
    constexpr std::size_t kBurstMinimum     = 415 * 10;
    constexpr std::size_t kOwnershipMinimum = poolSizeForQueues(kQraw + kQrec, 2, 1);

    STATIC_REQUIRE(kOwnershipMinimum > kBurstMinimum);

    // Y que el minimo declarado en pw::mem es exactamente ese, no otro parecido.
    STATIC_REQUIRE(pw::mem::kMinimumPoolSize == kOwnershipMinimum);

    WARN("minimo por rafaga de 10 s: " << kBurstMinimum
         << " | minimo por propiedad con Qraw+Qrec de 8192: " << kOwnershipMinimum
         << " (el valor por defecto de pw::config se verifica contra este numero en "
            "unit/config/test_pool_invariant.cpp)");
}

TEST_CASE("La politica drop-newest deja intacta la cola y no fuga referencias",
          "[mem][pool][ring][sizing]")
{
    // El contrato del productor cuando tryPush falla (04-ARCHITECTURE 4.3 v1.2):
    // no toca lo que ya hay, suelta la referencia de esa cola, cuenta el descarte
    // y sigue. Lo que NO puede pasar es perder el buffer entero ni corromper el
    // orden de lo que ya estaba encolado.
    BufferPool pool{poolSizeForQueues(kQueueCapacity, /*consumerCount=*/1)};
    auto       queue = std::make_unique<Queue>();

    std::vector<BufferHandle> encolados;
    std::uint64_t             descartados = 0;

    // Se llena la cola y se sigue empujando: los 32 primeros entran, el resto se
    // descartan por ser los mas nuevos.
    for (int i = 0; i < 100; ++i) {
        const auto handle = pool.acquire();
        REQUIRE(handle.valid());
        pool.setPayloadSize(handle, static_cast<std::size_t>(i) + 1);

        REQUIRE(pool.retain(handle));            // referencia para la cola
        if (queue->tryPush(handle)) {
            encolados.push_back(handle);
        } else {
            REQUIRE(pool.release(handle));       // se suelta la de la cola
            ++descartados;
        }
        REQUIRE(pool.release(handle));           // el productor suelta la suya
    }

    REQUIRE(encolados.size() == kQueueCapacity);
    REQUIRE(descartados == 100 - kQueueCapacity);
    REQUIRE(queue->size() == kQueueCapacity);

    // Lo que ya estaba encolado sigue intacto, en orden, y con su payload: el
    // rechazo no ha tocado nada.
    for (std::size_t i = 0; i < encolados.size(); ++i) {
        BufferHandle popped;
        REQUIRE(queue->tryPop(popped));
        REQUIRE(popped == encolados[i]);
        REQUIRE(pool.alive(popped));
        REQUIRE(pool.payloadSize(popped) == i + 1);
        REQUIRE(pool.release(popped));
    }

    // Y ninguna referencia se ha quedado colgando: los descartados volvieron al
    // pool en su momento, y los encolados al consumirse.
    REQUIRE(queue->empty());
    REQUIRE(pool.inUse() == 0);
    REQUIRE(pool.stats().released == 100);
    REQUIRE(pool.stats().rejectedReleases == 0);
    REQUIRE(pool.stats().exhausted == 0);
}

TEST_CASE("Sin colas disjuntas, un pool del tamano de una cola basta",
          "[mem][pool][sizing]")
{
    // El caso benigno, para no exagerar la conclusion: si los dos consumidores
    // van al dia, el pool nunca llega a tensionarse. El dimensionado no es para
    // el caso normal, es para el degradado.
    BufferPool pool{kQueueCapacity};

    auto qraw = std::make_unique<Queue>();
    auto qrec = std::make_unique<Queue>();

    std::uint64_t deliveredToPipeline = 0;
    std::uint64_t deliveredToRecorder = 0;

    for (int i = 0; i < 500; ++i) {
        const auto handle = pool.acquire();
        REQUIRE(handle.valid());

        pool.retain(handle);
        REQUIRE(qraw->tryPush(handle));
        pool.retain(handle);
        REQUIRE(qrec->tryPush(handle));
        pool.release(handle);

        BufferHandle consumed;
        while (qraw->tryPop(consumed)) {
            ++deliveredToPipeline;
            pool.release(consumed);
        }
        while (qrec->tryPop(consumed)) {
            ++deliveredToRecorder;
            pool.release(consumed);
        }
    }

    REQUIRE(deliveredToPipeline == 500);
    REQUIRE(deliveredToRecorder == 500);
    REQUIRE(pool.stats().exhausted == 0);
    REQUIRE(pool.inUse() == 0);
}
