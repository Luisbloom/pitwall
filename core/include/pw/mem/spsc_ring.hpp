#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::mem::SpscRing — cola circular de un productor y un consumidor (F1.1-02).
//
// Es la pieza que sostiene la fuerza F1 de la arquitectura: «la recepcion UDP no
// se bloquea jamas» (RNF-050). Por eso:
//
//   - Sin bloqueos y SIN MUTEX. Ninguna operacion puede dormir a T1.
//   - Sin asignaciones: el almacenamiento es un array fijo (RNF-013).
//   - Sin espera: tryPush y tryPop fallan y devuelven false; jamas esperan.
//
// UN productor y UN consumidor. No es MPMC ni MPSC: dos hilos empujando a la vez
// corrompen la cola en silencio. Cada instancia documenta quien es cada uno
// (04-ARCHITECTURE.md 4.3: Qraw de T1 a T2, Qrec de T1 a T3).
//
// ── SEMANTICA DE LA CAPACIDAD ────────────────────────────────────────────────
//
//   Una cola declarada SpscRing<T, N> almacena EXACTAMENTE N elementos.
//
// No N-1. Es una decision explicita: en lugar de reservar un hueco para
// distinguir «llena» de «vacia» —el truco clasico—, los indices son contadores
// MONOTONOS de 64 bits que solo se enmascaran al indexar. Entonces:
//
//   vacia  <=>  tail == head
//   llena  <=>  tail - head == N
//
// y las dos condiciones son distinguibles sin sacrificar un elemento. El coste
// es exigir que N sea potencia de dos, para que la mascara sustituya al modulo.
//
// Los contadores de 64 bits no desbordan en la practica: a 415 datagramas/s
// harian falta ~1.400 millones de anos.
//
// ── POLITICA DE DESBORDAMIENTO: DESCARTAR EL MAS NUEVO ──────────────────────
//
// Esta cola ofrece unicamente `tryPush`, que rechaza cuando esta llena. Es decir:
// descarta el MAS NUEVO. No es una limitacion que haya que rodear, es la politica
// decidida para Qraw (04-ARCHITECTURE.md 4.3, v1.2), y la razon es estructural:
//
//   El indice `head` es propiedad EXCLUSIVA del consumidor, y esa exclusividad es
//   justamente lo que permite prescindir de CAS en el camino caliente. Descartar
//   el mas antiguo significa avanzar `head`: si lo avanzara el productor, `head`
//   tendria dos escritores y esto dejaria de ser SPSC. Harian falta CAS sobre
//   `head` y una marca de secuencia por ranura para que el consumidor detectase
//   que le han sobrescrito la que estaba leyendo.
//
// Y con elementos que son referencias contadas -los de Qraw lo son- hay algo peor
// que la carrera de datos: sobrescribir una ranura obliga a SOLTAR la referencia
// que habia en ella, y el productor no puede saber si el consumidor esta leyendo
// justo ese buffer. La propiedad del elemento sobrescrito quedaria sin dueno.
//
// CONTRATO DEL PRODUCTOR cuando tryPush devuelve false:
//
//   1. no toca lo que ya hay en la cola;
//   2. suelta la referencia que habia retenido para esta cola;
//   3. incrementa el contador de descartes de esta cola;
//   4. sigue, sin bloquearse.
//
// Si algun dia una medicion real demostrase que hace falta recuperar frescura, la
// via prevista es que el CONSUMIDOR salte adelante cuando detecte retraso: eso
// conserva el modelo, porque `head` sigue teniendo un solo dueno. Es una
// posibilidad registrada, no un requisito.
//
// ── ALINEACION ──────────────────────────────────────────────────────────────
//
// Los indices del productor y del consumidor viven en LINEAS DE CACHE DISTINTAS.
// Sin esa separacion, cada escritura de uno invalida la linea que el otro esta
// leyendo -compartición falsa- y el rendimiento de la cola cae varias veces.
// Hay static_assert que lo verifica: es una propiedad que se pierde sola al
// anadir un miembro.
// ─────────────────────────────────────────────────────────────────────────────
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace pw::mem {

/// Tamano de linea de cache asumido. 64 B en x86-64 y en ARM64 corrientes.
/// No se usa std::hardware_destructive_interference_size porque su valor puede
/// diferir entre unidades de traduccion segun el compilador, y aqui aparece en
/// static_assert que deben ser estables.
inline constexpr std::size_t kCacheLineBytes = 64;

// MSVC avisa (C4324) de que estas estructuras se rellenan por el especificador
// de alineacion. Ese relleno es EXACTAMENTE lo que se busca: es lo que separa al
// productor del consumidor en lineas de cache distintas. Se silencia aqui, donde
// la intencion consta, y no en las opciones del compilador.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4324)
#endif

template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2, "SpscRing exige capacidad >= 2");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "SpscRing exige que la capacidad sea potencia de dos: la mascara "
                  "sustituye al modulo en el camino caliente");
    static_assert(std::is_default_constructible_v<T>,
                  "SpscRing preasigna su almacenamiento: T debe ser construible por defecto");
    static_assert(std::is_nothrow_move_assignable_v<T> || std::is_nothrow_copy_assignable_v<T>,
                  "SpscRing no puede propagar excepciones: T debe asignarse sin lanzar");

public:
    using value_type = T;

    SpscRing() noexcept = default;
    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&)                 = delete;
    SpscRing& operator=(SpscRing&&)      = delete;
    ~SpscRing()                          = default;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Encola. Devuelve false si la cola esta llena: NUNCA espera.
    /// Solo puede llamarlo el hilo productor.
    [[nodiscard]] bool tryPush(T value) noexcept
    {
        // tail_ solo lo escribe este hilo: relaxed basta para leerlo.
        const auto tail = producer_.tail.load(std::memory_order_relaxed);

        if (tail - producer_.cachedHead >= Capacity) {
            // La copia local del indice del consumidor dice «llena». Puede estar
            // obsoleta: se refresca antes de rendirse. Esta lectura es la unica
            // que cruza de hilo en el camino de escritura, y por eso se evita
            // mientras la copia local permita avanzar.
            //
            // acquire: sincroniza con el release del consumidor sobre head_, de
            // modo que si vemos que el hueco esta libre, tambien vemos que el
            // consumidor ya termino de leer el elemento que habia alli.
            producer_.cachedHead = consumer_.head.load(std::memory_order_acquire);
            if (tail - producer_.cachedHead >= Capacity) { return false; }
        }

        slots_[tail & kMask] = std::move(value);

        // release: publica la escritura de la ranura. El consumidor que lea este
        // valor de tail_ con acquire vera el elemento completo, no a medias.
        producer_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Desencola en `out`. Devuelve false si la cola esta vacia: NUNCA espera.
    /// Solo puede llamarlo el hilo consumidor.
    [[nodiscard]] bool tryPop(T& out) noexcept
    {
        // head_ solo lo escribe este hilo: relaxed basta.
        const auto head = consumer_.head.load(std::memory_order_relaxed);

        if (head == consumer_.cachedTail) {
            // acquire: emparejado con el release del productor sobre tail_.
            // Es lo que hace visible el contenido de la ranura.
            consumer_.cachedTail = producer_.tail.load(std::memory_order_acquire);
            if (head == consumer_.cachedTail) { return false; }
        }

        out = std::move(slots_[head & kMask]);

        // release: publica «esta ranura ya esta libre». Emparejado con el
        // acquire del productor sobre head_.
        consumer_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Numero aproximado de elementos. Solo para metricas y diagnostico: entre
    /// la lectura de los dos indices, los dos hilos pueden haber avanzado.
    [[nodiscard]] std::size_t size() const noexcept
    {
        const auto tail = producer_.tail.load(std::memory_order_acquire);
        const auto head = consumer_.head.load(std::memory_order_acquire);
        return static_cast<std::size_t>(tail - head);
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] bool full()  const noexcept { return size() >= Capacity; }

    /// Total historico de elementos encolados y desencolados. Para metricas.
    [[nodiscard]] std::uint64_t pushed() const noexcept
    {
        return producer_.tail.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t popped() const noexcept
    {
        return consumer_.head.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    /// Indice del productor y su copia del indice del consumidor. Juntos porque
    /// los toca el mismo hilo; aislados del consumidor por una linea de cache.
    struct alignas(kCacheLineBytes) Producer {
        std::atomic<std::uint64_t> tail{0};
        std::uint64_t              cachedHead{0};   ///< solo lo toca el productor
    };

    struct alignas(kCacheLineBytes) Consumer {
        std::atomic<std::uint64_t> head{0};
        std::uint64_t              cachedTail{0};   ///< solo lo toca el consumidor
    };

    Producer              producer_{};
    Consumer              consumer_{};
    std::array<T, Capacity> slots_{};

    // ── La propiedad que se pierde sola al anadir un miembro ─────────────────
    static_assert(alignof(Producer) == kCacheLineBytes,
                  "El bloque del productor debe estar alineado a linea de cache");
    static_assert(alignof(Consumer) == kCacheLineBytes,
                  "El bloque del consumidor debe estar alineado a linea de cache");
    static_assert(sizeof(Producer) == kCacheLineBytes,
                  "El bloque del productor debe ocupar una linea de cache entera: "
                  "si comparte linea con el consumidor hay compartición falsa");
    static_assert(sizeof(Consumer) == kCacheLineBytes,
                  "El bloque del consumidor debe ocupar una linea de cache entera");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "SpscRing exige indices atomicos sin bloqueos");
};

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

} // namespace pw::mem
