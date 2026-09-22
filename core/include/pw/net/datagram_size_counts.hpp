#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::net::DatagramSizeCounts — contadores por tamano (F1.1-08, RF-008).
//
// Un contador por cada longitud posible, de 0 bytes al tamano del buffer del
// pool. Exacto, no por cubos: cada paquete de F1 tiene un tamano FIJO, y lo que
// Diagnostics necesita es comparar tamanos observados con tamanos esperados
// (CU-12). Un histograma de cubos mezclaria justo lo que hay que distinguir.
//
// Cuenta TODO lo que se lee del socket, truncados incluidos: la longitud de un
// datagrama invalido es precisamente la pista de por que lo es. Los oversized no
// entran: no cabian en el buffer, asi que su longitud real no se conoce, y ya
// tienen su propio contador por causa.
//
// La tabla se reserva UNA vez, al construir (con 2048 B de buffer son 2049
// contadores, 16 KiB). Registrar es un fetch_add relajado sobre una posicion ya
// existente: ni busqueda, ni asignacion.
//
//   Escribe   SOLO T1, con record().
//   Lee       cualquier hilo; snapshot() asigna y no se llama desde T1.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "pw/obs/metrics.hpp"

namespace pw::net {

struct SizeCount {
    std::size_t   bytes{0};
    std::uint64_t datagrams{0};
};

class DatagramSizeCounts {
public:
    /// Reserva un contador por longitud en [0, maxBytes]. Arranque: asigna.
    explicit DatagramSizeCounts(std::size_t maxBytes);

    DatagramSizeCounts(const DatagramSizeCounts&)            = delete;
    DatagramSizeCounts& operator=(const DatagramSizeCounts&) = delete;

    /// SOLO T1.
    void record(const std::size_t bytes) noexcept
    {
        if (bytes <= maxBytes_) {
            counts_[bytes].fetch_add(1, std::memory_order_relaxed);
        } else {
            // No deberia ocurrir: el socket nunca escribe mas que el buffer. Si
            // ocurre, se cuenta aparte en vez de perderlo o de salirse de la tabla.
            outOfRange_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::uint64_t count(const std::size_t bytes) const noexcept
    {
        return bytes <= maxBytes_ ? counts_[bytes].load(std::memory_order_relaxed) : 0;
    }

    [[nodiscard]] std::uint64_t outOfRange() const noexcept
    {
        return outOfRange_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t maxBytes() const noexcept { return maxBytes_; }

    /// Solo las longitudes observadas al menos una vez, de menor a mayor.
    /// Asigna: NO se llama desde T1.
    [[nodiscard]] std::vector<SizeCount> snapshot() const;

private:
    std::size_t                                  maxBytes_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> counts_;
    std::atomic<std::uint64_t>                   outOfRange_{0};
};

/// udp_packets_by_size_total{bytes=N}, solo para las longitudes observadas.
/// Asigna: NO se llama desde T1.
void appendMetricSamples(const DatagramSizeCounts& sizes, std::vector<obs::MetricSample>& out);

} // namespace pw::net
