#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::net::SourceRegistry — tabla de origenes por indice (F1.1-09, RF-009) y
// contadores por origen (F1.1-08, RF-008).
//
// Cada datagrama llega con la IP y el puerto de quien lo envio. La tabla le da
// a cada origen distinto un indice pequeno y estable, y cuenta por indice los
// datagramas y los bytes. Es lo que resuelve el caso real de dos emisores en el
// mismo puerto [S9]: primero hay que poder distinguirlos.
//
// ── IDENTIDAD ───────────────────────────────────────────────────────────────
//
// La identidad de un origen es el par (IP, puerto), y nada mas. Misma IP con
// otro puerto es OTRO origen; mismo puerto desde otra IP, tambien.
//
// Se guarda en binario, en un uint64:
//
//   bit 48         ocupado (1). Hace que 0.0.0.0:0 sea representable y que una
//                  ranura vacia (clave 0) nunca coincida con un origen real
//   bits 47..16    la IPv4, en orden de red
//   bits 15..0     el puerto
//
// El texto «192.168.1.50:20777» NO existe en T1: solo se construye al leer la
// tabla desde fuera, en snapshot().
//
// ── CAPACIDAD: 8, FIJA ──────────────────────────────────────────────────────
//
// Lo normal es un origen (el juego). El caso real documentado son dos. Ocho
// cubre el juego, una segunda consola, un reenviador y alguna herramienta con
// margen de sobra. Las ocho claves ocupan exactamente una linea de cache, asi
// que buscar es recorrer 64 bytes contiguos: mas barato que calcular un hash, y
// sin ninguna estructura que pueda crecer.
//
// ── TABLA LLENA ─────────────────────────────────────────────────────────────
//
// Un noveno origen NO desplaza a nadie: desalojar cambiaria el significado de
// un indice que otro componente ya puede estar usando. El datagrama se sigue
// recibiendo y entregando -la tabla no filtra- pero recibe el indice
// kUnregisteredSource y se cuenta en una ranura de desbordamiento propia. Nunca
// bloquea, nunca asigna y nunca toca las entradas existentes. El receptor marca
// ademas un suceso para que quede registrado fuera de T1.
//
// ── QUIEN ESCRIBE Y QUIEN LEE ───────────────────────────────────────────────
//
//   Escribe   UN SOLO hilo: T1. resolve() y record() son exclusivos suyos.
//             Llamarlos desde dos hilos a la vez rompe el contrato.
//   Lee       cualquier hilo: size(), datagrams(), bytes(), snapshot().
//
// La publicacion de una entrada nueva sigue este orden, y es lo que hace
// coherente la lectura desde fuera sin ningun mutex:
//
//   1. T1 escribe la clave en su ranura (dato normal, no atomico).
//   2. T1 publica el nuevo tamano con store(release).
//
// Un lector hace load(acquire) del tamano y solo mira las ranuras por debajo de
// el. Toda clave que ve esta escrita del todo, y ya no cambiara nunca: una
// entrada publicada es inmutable. Los contadores son atomicos relajados, igual
// que el resto de metricas: cada uno es exacto por separado, pero dos contadores
// leidos seguidos pueden no corresponder al mismo instante.
// ─────────────────────────────────────────────────────────────────────────────
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "pw/core/types.hpp"
#include "pw/net/endpoint.hpp"
#include "pw/obs/metrics.hpp"

namespace pw::net {

/// Indice de un origen dentro de la tabla: 0..kMaxSources-1, o
/// kUnregisteredSource si la tabla estaba llena cuando aparecio.
using SourceIndex = StrongId<std::uint8_t, struct SourceIndexTag>;

inline constexpr std::size_t kMaxSources = 8;
inline constexpr SourceIndex kUnregisteredSource{0xFF};

static_assert(kMaxSources < 0xFF, "el indice 0xFF esta reservado para kUnregisteredSource");

/// Lectura de un origen, para Diagnostics y el volcado de metricas.
struct SourceSnapshot {
    SourceIndex   index{kUnregisteredSource};
    Ipv4Endpoint  endpoint{};
    std::uint64_t datagrams{0};
    std::uint64_t bytes{0};
};

// MSVC avisa (C4324) de que la clase se rellena por el alignas de las claves.
// Ese relleno es lo que se busca: las ocho claves en una sola linea de cache.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4324)
#endif

class SourceRegistry {
public:
    SourceRegistry() noexcept = default;
    SourceRegistry(const SourceRegistry&)            = delete;
    SourceRegistry& operator=(const SourceRegistry&) = delete;

    /// Clave binaria de un origen. Nunca es 0.
    [[nodiscard]] static constexpr std::uint64_t keyOf(const Ipv4Endpoint& origin) noexcept
    {
        return kOccupiedBit | (static_cast<std::uint64_t>(origin.toNetworkOrder()) << 16)
               | static_cast<std::uint64_t>(origin.port());
    }

    [[nodiscard]] static constexpr Ipv4Endpoint endpointOf(const std::uint64_t key) noexcept
    {
        return Ipv4Endpoint::fromNetworkOrder(static_cast<std::uint32_t>((key >> 16) & 0xFFFF'FFFFULL),
                                              static_cast<std::uint16_t>(key & 0xFFFFULL));
    }

    /// SOLO T1. Devuelve el indice del origen y lo registra si es nuevo y cabe.
    /// Si la tabla esta llena y el origen es nuevo, devuelve kUnregisteredSource
    /// sin modificar nada.
    [[nodiscard]] SourceIndex resolve(const Ipv4Endpoint& origin) noexcept
    {
        const auto key   = keyOf(origin);
        // T1 es el unico que escribe count_: su propia lectura puede ser relajada.
        const auto count = count_.load(std::memory_order_relaxed);

        for (std::size_t i = 0; i < count; ++i) {
            if (keys_[i] == key) { return SourceIndex{static_cast<std::uint8_t>(i)}; }
        }

        if (count == kMaxSources) { return kUnregisteredSource; }

        keys_[count] = key;                                    // 1. el dato
        count_.store(count + 1, std::memory_order_release);    // 2. la publicacion
        return SourceIndex{static_cast<std::uint8_t>(count)};
    }

    /// SOLO T1. Cuenta un datagrama leido del socket para ese origen. Con
    /// kUnregisteredSource cuenta en la ranura de desbordamiento.
    void record(const SourceIndex index, const std::size_t bytes) noexcept
    {
        // Cualquier indice fuera de la tabla cae en el desbordamiento: nunca se
        // escribe fuera del array, aunque llegue un indice que resolve() no dio.
        auto& slot = index.get() < kMaxSources ? slots_[index.get()] : overflow_;
        slot.datagrams.fetch_add(1, std::memory_order_relaxed);
        slot.bytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    /// Origenes registrados. Seguro desde cualquier hilo.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return count_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return kMaxSources; }

    /// Datagramas de ese origen; con kUnregisteredSource, los del desbordamiento.
    [[nodiscard]] std::uint64_t datagrams(const SourceIndex index) const noexcept
    {
        return slotOf(index).datagrams.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t bytes(const SourceIndex index) const noexcept
    {
        return slotOf(index).bytes.load(std::memory_order_relaxed);
    }

    /// Endpoint de un indice publicado. Fuera de rango devuelve un endpoint vacio.
    [[nodiscard]] Ipv4Endpoint endpoint(SourceIndex index) const noexcept;

    /// Instantanea de los origenes registrados, en orden de indice. Asigna: NO
    /// se llama desde T1.
    [[nodiscard]] std::vector<SourceSnapshot> snapshot() const;

private:
    static constexpr std::uint64_t kOccupiedBit = 1ULL << 48;

    /// Sin alineacion a linea de cache: hay un unico escritor, asi que no hay
    /// dos hilos peleando por la misma linea que separar.
    struct Slot {
        std::atomic<std::uint64_t> datagrams{0};
        std::atomic<std::uint64_t> bytes{0};
    };

    [[nodiscard]] const Slot& slotOf(const SourceIndex index) const noexcept
    {
        return index.get() < kMaxSources ? slots_[index.get()] : overflow_;
    }

    /// Escritas solo por T1 y solo por encima de count_; inmutables una vez
    /// publicadas. Juntas ocupan una linea de cache.
    alignas(64) std::array<std::uint64_t, kMaxSources> keys_{};
    std::atomic<std::size_t>                           count_{0};

    std::array<Slot, kMaxSources> slots_{};
    Slot                          overflow_{};
};

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

/// Metricas por origen, con el texto del origen como etiqueta:
///
///   udp_packets_by_source_total{source=192.168.1.50:20777}
///   udp_bytes_by_source_total{source=192.168.1.50:20777}
///
/// y las mismas dos con source=unregistered para el desbordamiento, que se
/// emiten SIEMPRE -tambien a cero- para que «la tabla no se ha llenado» sea un
/// dato visible y no una ausencia. Asigna: NO se llama desde T1.
void appendMetricSamples(const SourceRegistry& sources, std::vector<obs::MetricSample>& out);

} // namespace pw::net
