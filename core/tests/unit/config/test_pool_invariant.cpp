// LA INVARIANTE QUE IMPIDE QUE LA DISCREPANCIA REAPAREZCA (F1.1-01).
//
// El valor por defecto del pool vive en pw::config; la regla que lo restringe se
// deduce en pw::mem, a partir del modelo de propiedad de los buffers. Los dos
// modulos no pueden depender uno del otro -pw::config no conoce pw::mem, y esta
// bien que no lo conozca (grafo de modulos, RNF-084)-, asi que el numero esta
// escrito en los dos sitios.
//
// Esta prueba es el unico lugar donde los dos se encuentran, y falla en cuanto
// se separan. Es exactamente la prueba que no existia cuando el documento decia
// «poolSize = 4096» en una seccion y «poolSize >= 4150» en otra.
#include <catch2/catch_test_macros.hpp>

#include "pw/config/config.hpp"
#include "pw/mem/buffer_pool.hpp"

TEST_CASE("El pool por defecto cumple la regla deducida del modelo de propiedad",
          "[config][mem][sizing][RNF-013]")
{
    // 1. Las dos declaraciones del minimo coinciden.
    STATIC_REQUIRE(pw::config::kMinimumPoolSize == pw::mem::kMinimumPoolSize);

    // 2. El minimo es el que deduce la regla, y no otro numero parecido.
    STATIC_REQUIRE(pw::mem::kMinimumPoolSize ==
                   pw::mem::poolSizeForQueues(pw::mem::kQrawCapacity + pw::mem::kQrecCapacity,
                                              /*consumerCount=*/2, /*producerInFlight=*/1));

    // 3. Y el valor por defecto lo cumple.
    STATIC_REQUIRE(pw::config::kDefaultPoolSize >= pw::mem::kMinimumPoolSize);
    REQUIRE(pw::config::defaults().net.poolSize >= pw::mem::kMinimumPoolSize);
}

TEST_CASE("El minimo no se confunde con el minimo por rafaga", "[config][mem][sizing]")
{
    // Las dos restricciones existen y son distintas; manda la mayor. Dejarlo
    // escrito en una prueba evita volver a elegir la equivocada.
    constexpr std::size_t burstMinimum = 415 * 10;

    STATIC_REQUIRE(pw::mem::kMinimumPoolSize > burstMinimum);
    STATIC_REQUIRE(pw::config::kDefaultPoolSize > burstMinimum);
}

TEST_CASE("Un pool dimensionado por defecto aguanta el peor caso real",
          "[config][mem][sizing][RNF-013]")
{
    // Comprobacion aritmetica del caso que decide el dimensionado: las dos colas
    // llenas de buffers DISJUNTOS, cada consumidor con uno en la mano y el
    // productor rellenando otro.
    constexpr std::size_t worstCaseLiveBuffers =
        pw::mem::kQrawCapacity + pw::mem::kQrecCapacity + 2 + 1;

    STATIC_REQUIRE(pw::config::kDefaultPoolSize >= worstCaseLiveBuffers);

    // Y el margen que queda, para que se vea que es holgura y no casualidad.
    STATIC_REQUIRE(pw::config::kDefaultPoolSize - worstCaseLiveBuffers > 4000);
}
