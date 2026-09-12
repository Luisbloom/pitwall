#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::mem::BufferPool — buffers preasignados con recuento de referencias
// (F1.1-01, RNF-013, 04-ARCHITECTURE.md 4.4).
//
// ── EL PROBLEMA QUE RESUELVE ────────────────────────────────────────────────
//
// Un datagrama que llega por el socket alimenta a DOS consumidores: el pipeline
// que lo decodifica (T2) y el grabador que lo escribe en la captura (T3). Los
// dos leen el MISMO payload, y ninguno lo copia: copiar 1.470 B a 415
// datagramas/s por consumidor seria trabajo puro en el camino caliente.
//
// Si no se copia, alguien tiene que decidir cuando el buffer vuelve a estar
// libre. No puede decidirlo T1, que ya se ha ido a por el siguiente datagrama,
// ni puede decidirlo uno solo de los consumidores, porque el otro quiza siga
// leyendo. Lo decide un RECUENTO DE REFERENCIAS: el buffer vuelve al pool
// cuando lo suelta el ultimo que lo tenia.
//
// ── EL MODELO DE PROPIEDAD ──────────────────────────────────────────────────
//
//   1. T1 hace acquire()            -> refs = 1   (la referencia del productor)
//   2. T1 hace retain() por cada consumidor al que va a encolarlo
//   3. T1 hace release() de SU referencia en cuanto ha encolado
//   4. Cada consumidor hace release() cuando termina de leer
//   5. El release() que lleva refs a 0 devuelve el buffer al pool
//
// El paso 3 es el que evita una fuga sutil: si T1 se quedara con su referencia,
// el buffer no volveria nunca. Y hacer retain() ANTES de encolar, y no despues,
// es lo que evita la carrera contraria: el consumidor podria haber consumido y
// soltado el buffer antes de que el productor llegara a incrementar.
//
//   const auto handle = pool.acquire();          // refs = 1
//   pool.retain(handle);                         // refs = 2  (para Qraw)
//   pool.retain(handle);                         // refs = 3  (para Qrec)
//   if (!qraw.tryPush(handle)) pool.release(handle);   // la cola lo rechazo
//   if (!qrec.tryPush(handle)) pool.release(handle);
//   pool.release(handle);                        // T1 suelta la suya
//
// ── EL PROTOCOLO DE ESTADO: UNA SOLA PALABRA ATOMICA ────────────────────────
//
// Generacion y recuento de referencias viven en el MISMO uint64_t:
//
//     bits 63..16   generacion   (48 bits; sube en cada muerte del buffer)
//     bits 15..0    referencias  (16 bits; 0 = muerto, maximo 65.535)
//
// Que compartan palabra no es una optimizacion: es lo que hace que la muerte
// del buffer sea ATOMICA. La ultima referencia que se suelta ejecuta un solo
// compare_exchange que a la vez pone refs a 0 y sube la generacion, asi que:
//
//   - un handle antiguo deja de ser valido EN EL MISMO INSTANTE en que el
//     buffer muere, no cuando alguien vuelve a adquirirlo;
//   - retain() sobre un handle muerto no puede resucitarlo: el CAS ve refs == 0
//     o una generacion distinta, y se rechaza;
//   - un release() duplicado no puede hacer underflow: se rechaza antes de
//     restar, y se contabiliza;
//   - no hay ventana entre «el buffer ha muerto» y «el handle ha caducado»,
//     porque son el mismo cambio de estado.
//
// Es, en esencia, el protocolo de weak_ptr::lock: comprobar y adquirir en un
// unico paso atomico, en lugar de comprobar y despues adquirir.
//
// El buffer se devuelve a la lista de libres DESPUES de ese CAS, de modo que
// nunca es reciclable mientras un handle antiguo pueda considerarlo valido.
//
// ── POR QUE 48 BITS DE GENERACION Y NO 32 ───────────────────────────────────
//
// La lista de libres es LIFO: el ultimo buffer devuelto es el primero que se
// vuelve a entregar. En regimen normal -el pool casi vacio, un datagrama cada
// 2,4 ms- eso significa que se reutiliza SIEMPRE LA MISMA RANURA. No hay reparto
// uniforme entre los 20.480 buffers, y suponerlo es el error que hace que las
// cuentas salgan tranquilizadoras:
//
//     32 bits, reparto uniforme (falso)   2^32 / (415/20480)   ~ 6.800 anos
//     32 bits, peor caso real (LIFO)      2^32 / 415           ~ 120 DIAS
//     48 bits, peor caso real             2^48 / 415           ~ 21.500 anos
//
// 120 dias es un numero alcanzable: una sesion de PitWall abierta durante meses
// en un PC que no se apaga. Y dar la vuelta significa que un handle antiguo
// vuelve a parecer valido sobre un buffer que ya no es el suyo, que es
// exactamente el fallo silencioso que este protocolo existe para impedir.
//
// Los 16 bits que quedan para el recuento permiten 65.535 referencias
// simultaneas sobre un mismo buffer. PitWall usa tres -productor, Qraw y Qrec-;
// el margen es de cuatro ordenes de magnitud. Y retain() rechaza explicitamente
// al llegar al maximo, ANTES de que el incremento pueda propagarse por acarreo
// hacia los bits de la generacion.
//
// La generacion empieza en 1 y NUNCA vale 0: la 0 queda reservada para «ninguna
// generacion», que es la de un BufferHandle por defecto. Si algun dia la
// generacion diera la vuelta, el incremento salta del maximo al 1, de modo que
// una generacion valida 0 no puede producirse por construccion y no depende de
// que el desbordamiento sea improbable.
//
// ── POR QUE UN INDICE Y NO UN PUNTERO ───────────────────────────────────────
//
// BufferHandle es un indice al pool mas una generacion, no un puntero. Un uso
// despues de soltar el buffer no lee memoria ajena: se detecta y se cuenta. Con
// un puntero crudo, el mismo error seria una lectura de datos de otro datagrama
// que parecerian validos.
//
// ── CONCURRENCIA ────────────────────────────────────────────────────────────
//
// Sin mutex y sin bloqueos. La lista de libres es una pila de Treiber con
// cabecera etiquetada: el contador de etiqueta sube en cada operacion, lo que
// elimina el problema ABA. Las ranuras nunca se destruyen -viven en un array
// que dura lo que el pool-, asi que leer el enlace de una ranura reciclada es
// seguro aunque el valor leido este obsoleto.
//
// retain(), release() y las lecturas son seguras desde cualquier hilo.
// acquire() esta pensado para T1, pero tambien es seguro desde varios.
//
// CONTRATO DE LAS LECTURAS: bytes(), payload() y payloadSize() exigen que quien
// llama TENGA una referencia viva. Validan el handle para convertir un error en
// un vacio contabilizado en lugar de en una lectura de datos ajenos, pero no
// pueden proteger de soltar la referencia y seguir leyendo despues.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace pw::mem {

/// Tamano por defecto de cada buffer. Cubre con holgura el paquete mayor
/// (1.470 B en 2026) y absorbe un formato futuro mas grande (04-ARCHITECTURE 4.4).
inline constexpr std::size_t kDefaultBufferBytes = 2048;

/// Capacidades de las colas del camino caliente (04-ARCHITECTURE.md 4.3).
/// Viven aqui, en un solo sitio, porque son las que fijan el tamano del pool
/// (RNF-110: el limite es un parametro del formato, no una constante repartida).
inline constexpr std::size_t kQrawCapacity = 8192;   ///< T1 -> T2, pipeline
inline constexpr std::size_t kQrecCapacity = 8192;   ///< T1 -> T3, grabador

/// Referencia a un buffer del pool: indice + generacion, trivialmente copiable
/// para que viaje por una SpscRing sin coste.
///
/// La generacion es de 48 bits, asi que el handle ocupa 16 B y no 8. Se
/// consideraron las dos alternativas para conservar los 8 B:
///
///   - empaquetar indice y generacion en un solo uint64 con el indice en 16
///     bits: limitaria el pool a 65.535 buffers, y el minimo exigible ya es
///     16.387; con colas de 32.768 -un valor nada absurdo- se pasaria del
///     limite. Un techo que puede alcanzarse no compensa 8 bytes.
///   - guardar solo 32 bits de generacion en el handle: no sirve de nada. Lo
///     que tiene que ser distinguible entre dos vidas de la ranura es el
///     HANDLE; si el handle solo lleva 32 bits, ampliar la ranura a 48 no
///     cambia nada.
///
/// Lo que cuestan esos 8 bytes de mas: 64 KiB adicionales por cola de 8.192
/// huecos. Irrelevante frente al riesgo que eliminan.
class BufferHandle {
public:
    static constexpr std::uint32_t kNullIndex = 0xFFFF'FFFFu;

    constexpr BufferHandle() noexcept = default;
    constexpr BufferHandle(std::uint32_t index, std::uint64_t generation) noexcept
        : generation_{generation}, index_{index}
    {
    }

    [[nodiscard]] constexpr bool          valid()      const noexcept { return index_ != kNullIndex; }
    [[nodiscard]] constexpr std::uint32_t index()      const noexcept { return index_; }
    [[nodiscard]] constexpr std::uint64_t generation() const noexcept { return generation_; }

    constexpr bool operator==(const BufferHandle&) const noexcept = default;

private:
    std::uint64_t generation_{0};   ///< 0 no es una generacion valida
    std::uint32_t index_{kNullIndex};
};

static_assert(std::is_trivially_copyable_v<BufferHandle>,
              "BufferHandle viaja por colas sin bloqueos: debe ser trivialmente copiable");
static_assert(sizeof(BufferHandle) == 16,
              "BufferHandle: 8 B de generacion de 48 bits mas 4 B de indice, con relleno");

/// Maximo de referencias simultaneas sobre un buffer. retain() lo rechaza al
/// llegar aqui, antes de que el incremento pueda acarrear hacia la generacion.
inline constexpr std::uint32_t kMaxBufferRefs = 0xFFFFu;

/// Estadisticas del pool, para *UDP Diagnostics* (RF-806) y para las pruebas.
struct BufferPoolStats {
    std::uint64_t acquired{0};         ///< acquire() con exito
    std::uint64_t released{0};         ///< buffers devueltos al pool (refs llego a 0)
    std::uint64_t exhausted{0};        ///< acquire() sin buffers libres (RNF-013)
    std::uint64_t stale{0};            ///< accesos con un handle muerto o caducado
    std::uint64_t rejectedRetains{0};  ///< retain() sobre un handle muerto
    std::uint64_t rejectedReleases{0}; ///< release() duplicado o sobre un handle muerto
    std::size_t   inUse{0};            ///< buffers actualmente fuera del pool
};

// MSVC avisa (C4324) de que estas estructuras se rellenan por el especificador
// de alineacion. Ese relleno es EXACTAMENTE lo que se busca: es lo que separa en
// lineas de cache distintas datos que tocan hilos distintos. Se silencia aqui,
// donde la intencion consta, y no en las opciones del compilador.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4324)
#endif

class BufferPool {
public:
    /// Reserva `count` buffers de `bufferBytes` cada uno. Es la UNICA asignacion
    /// de memoria del pool: despues de construirlo no se asigna nada mas.
    BufferPool(std::size_t count, std::size_t bufferBytes = kDefaultBufferBytes);

    BufferPool(const BufferPool&)            = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    ~BufferPool();

    /// Toma un buffer libre con refs = 1. Devuelve un handle invalido si el pool
    /// esta agotado, y lo contabiliza. NUNCA espera y NUNCA asigna memoria.
    [[nodiscard]] BufferHandle acquire() noexcept;

    /// Anade una referencia. Precondicion: quien llama ya tiene una.
    /// Devuelve false -y lo contabiliza- en dos casos:
    ///   - el handle ya estaba muerto: un buffer soltado NO puede resucitarse;
    ///   - el recuento ya esta en kMaxBufferRefs: incrementar acarrearia hacia
    ///     los bits de la generacion y corromperia la identidad del buffer.
    bool retain(BufferHandle handle) noexcept;

    /// Suelta una referencia. La que llega a cero mata el buffer, invalida todos
    /// sus handles y lo devuelve al pool, todo en un unico paso atomico.
    /// Devuelve false -y lo contabiliza- si el handle ya estaba muerto: un
    /// release() duplicado no resta nada.
    bool release(BufferHandle handle) noexcept;

    /// Memoria del buffer, para escribir en ella. Vacio si el handle esta muerto.
    [[nodiscard]] std::span<std::byte>       bytes(BufferHandle handle) noexcept;
    [[nodiscard]] std::span<const std::byte> bytes(BufferHandle handle) const noexcept;

    /// Bytes utiles escritos en el buffer: la longitud real del datagrama.
    /// Lo fija el productor antes de encolar; el encolado es quien lo publica.
    void                      setPayloadSize(BufferHandle handle, std::size_t size) noexcept;
    [[nodiscard]] std::size_t payloadSize(BufferHandle handle) const noexcept;

    /// Payload util, ya recortado a payloadSize(). Es lo que leen T2 y T3.
    [[nodiscard]] std::span<const std::byte> payload(BufferHandle handle) const noexcept;

    /// Cierto si el buffer sigue vivo Y el handle sigue siendo el suyo.
    [[nodiscard]] bool alive(BufferHandle handle) const noexcept;

    /// Referencias vivas. 0 si el handle esta muerto. Para pruebas y diagnostico.
    [[nodiscard]] std::uint32_t refCount(BufferHandle handle) const noexcept;

    /// Generacion actual de la ranura, de 48 bits. Solo para pruebas y depuracion.
    [[nodiscard]] std::uint64_t generationOf(std::uint32_t index) const noexcept;

    [[nodiscard]] std::size_t capacity()    const noexcept { return count_; }
    [[nodiscard]] std::size_t bufferBytes() const noexcept { return bufferBytes_; }
    [[nodiscard]] std::size_t available()   const noexcept;
    [[nodiscard]] std::size_t inUse()       const noexcept;

    [[nodiscard]] BufferPoolStats stats() const noexcept;

    /// Memoria total reservada, para el presupuesto de RNF-011.
    [[nodiscard]] std::size_t reservedBytes() const noexcept;

private:
    /// Metadatos de un buffer. Cada ranura ocupa su propia linea de cache: dos
    /// buffers en vuelo en hilos distintos no deben compartir linea, o sus
    /// recuentos de referencias se pelearian por ella.
    struct alignas(64) Slot {
        /// bits 63..16 generacion (48), bits 15..0 referencias (16). Ver el
        /// protocolo de estado en la cabecera del fichero.
        std::atomic<std::uint64_t> state{0};
        std::atomic<std::uint32_t> payloadSize{0};
        std::atomic<std::uint32_t> next{BufferHandle::kNullIndex};  ///< lista de libres
    };

    static constexpr std::uint64_t kRefBits        = 16;
    static constexpr std::uint64_t kRefMask        = 0xFFFFULL;
    static constexpr std::uint64_t kGenerationMask = 0xFFFF'FFFF'FFFFULL;   // 48 bits

    static constexpr std::uint64_t makeState(std::uint64_t generation,
                                             std::uint32_t refs) noexcept
    {
        return ((generation & kGenerationMask) << kRefBits)
               | (static_cast<std::uint64_t>(refs) & kRefMask);
    }
    static constexpr std::uint64_t generationOfState(std::uint64_t state) noexcept
    {
        return (state >> kRefBits) & kGenerationMask;
    }
    static constexpr std::uint32_t refsOfState(std::uint64_t state) noexcept
    {
        return static_cast<std::uint32_t>(state & kRefMask);
    }
    /// Generacion siguiente, saltandose el 0 si la cuenta diera la vuelta. Que
    /// la generacion 0 no pueda producirse no depende de que el desbordamiento
    /// sea improbable: depende de esta linea.
    static constexpr std::uint64_t nextGeneration(std::uint64_t generation) noexcept
    {
        const auto next = (generation + 1) & kGenerationMask;
        return next == 0 ? 1 : next;
    }

    /// Ranura del handle sin comprobar su estado: solo limites.
    [[nodiscard]] Slot*       rawSlot(BufferHandle handle) noexcept;
    [[nodiscard]] const Slot* rawSlot(BufferHandle handle) const noexcept;

    /// Comprueba que el buffer sigue vivo y que el handle sigue siendo el suyo.
    /// Contabiliza el acceso caducado si no lo es.
    [[nodiscard]] bool isLive(BufferHandle handle) const noexcept;

    void                        pushFree(std::uint32_t index) noexcept;
    [[nodiscard]] std::uint32_t popFree() noexcept;

    static constexpr std::uint64_t pack(std::uint32_t index, std::uint32_t tag) noexcept
    {
        return (static_cast<std::uint64_t>(tag) << 32) | index;
    }
    static constexpr std::uint32_t indexOf(std::uint64_t packed) noexcept
    {
        return static_cast<std::uint32_t>(packed & 0xFFFF'FFFFu);
    }
    static constexpr std::uint32_t tagOf(std::uint64_t packed) noexcept
    {
        return static_cast<std::uint32_t>(packed >> 32);
    }

    struct AlignedDelete {
        void operator()(std::byte* p) const noexcept;
    };

    std::size_t                                 count_;
    std::size_t                                 bufferBytes_;
    std::unique_ptr<std::byte[], AlignedDelete> storage_;
    std::unique_ptr<Slot[]>                     slots_;

    /// Cabecera etiquetada de la pila de libres: 32 bits de indice y 32 de
    /// etiqueta. La etiqueta sube en cada operacion y es lo que elimina el ABA.
    alignas(64) std::atomic<std::uint64_t> freeHead_;

    alignas(64) std::atomic<std::uint64_t> acquired_{0};
    std::atomic<std::uint64_t>             released_{0};
    std::atomic<std::uint64_t>             exhausted_{0};
    mutable std::atomic<std::uint64_t>     stale_{0};
    std::atomic<std::uint64_t>             rejectedRetains_{0};
    std::atomic<std::uint64_t>             rejectedReleases_{0};
    std::atomic<std::int64_t>              inUse_{0};

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "El protocolo de estado y la pila de libres exigen atomicos de "
                  "64 bits sin bloqueos");
};

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

/// Tamano de pool a partir del cual el POOL deja de ser el cuello de botella.
///
/// Cada cola que guarde handles mantiene su propia referencia, y dos colas que
/// avancen a ritmos distintos pueden acabar refiriendose a conjuntos de buffers
/// DISJUNTOS. En el peor caso, por tanto, los buffers vivos son la SUMA de las
/// capacidades de las colas, mas los que cada consumidor tenga en la mano y el
/// que el productor este rellenando.
///
/// Por debajo de ese numero, el pool se agota ANTES de que ninguna cola se
/// llene, y las politicas de desbordamiento por cola -que son las documentadas
/// y las contabilizadas- no llegan a aplicarse (04-ARCHITECTURE 4.3).
[[nodiscard]] constexpr std::size_t poolSizeForQueues(std::size_t queueCapacitySum,
                                                      std::size_t consumerCount,
                                                      std::size_t producerInFlight = 1) noexcept
{
    return queueCapacitySum + consumerCount + producerInFlight;
}

/// Minimo exigible al pool con la configuracion real de colas del proyecto.
inline constexpr std::size_t kMinimumPoolSize =
    poolSizeForQueues(kQrawCapacity + kQrecCapacity, /*consumerCount=*/2, /*producerInFlight=*/1);

} // namespace pw::mem
