#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::obs — metricas internas (F0-08, RF-800, OBS-01, RNF-100).
//
// Tres instrumentos y un registro consultable. Sin asignaciones en ejecucion,
// sin bloqueos al actualizar y con coste despreciable (< 1 % de CPU):
//
//   Counter   contador monotono creciente          (paquetes recibidos)
//   Gauge     valor instantaneo que sube y baja    (profundidad de cola)
//   Histogram cubos FIJOS preasignados             (latencia de decodificacion)
//
// Todas las actualizaciones usan orden relajado: el coste es de unos pocos
// nanosegundos y no introduce sincronizacion entre hilos. La consistencia
// exacta entre dos metricas distintas no es un requisito; el orden de magnitud
// y la tendencia si lo son.
//
// No hay ningun registro global: el registro es un objeto que se construye en
// pw::app y se pasa por referencia (09-BACKEND-PLAN 1.1, «ninguna variable
// global mutable»).
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "pw/core/time.hpp"

namespace pw::obs {

/// Contador monotono. Solo puede crecer.
class Counter {
public:
    void inc(std::uint64_t by = 1) noexcept { value_.fetch_add(by, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t value() const noexcept { return value_.load(std::memory_order_relaxed); }
    void reset() noexcept { value_.store(0, std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> value_{0};
};

/// Valor instantaneo, con signo: sube y baja.
class Gauge {
public:
    void set(std::int64_t v) noexcept { value_.store(v, std::memory_order_relaxed); }
    void add(std::int64_t delta) noexcept { value_.fetch_add(delta, std::memory_order_relaxed); }
    void sub(std::int64_t delta) noexcept { value_.fetch_sub(delta, std::memory_order_relaxed); }
    [[nodiscard]] std::int64_t value() const noexcept { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::int64_t> value_{0};
};

/// Histograma de cubos fijos, pensado para nanosegundos.
///
/// Los limites cubren de 1 us a 1 s en pasos de x4 aproximados, mas un cubo de
/// desbordamiento. Son suficientes para los umbrales del proyecto (p99 de
/// decodificacion < 50 us, latencia extremo a extremo p95 < 150 ms) sin
/// necesidad de cubos dinamicos ni de asignacion en ejecucion (OBS-02).
class Histogram {
public:
    static constexpr std::size_t kBucketCount = 16;

    /// Limite superior inclusivo de cada cubo, en nanosegundos.
    static constexpr std::array<std::uint64_t, kBucketCount - 1> kUpperBoundsNs{
        1'000ULL,        // 1 us
        2'000ULL,        // 2 us
        5'000ULL,        // 5 us
        10'000ULL,       // 10 us
        25'000ULL,       // 25 us
        50'000ULL,       // 50 us   <- umbral RNF-002 de decodificacion
        100'000ULL,      // 100 us
        250'000ULL,      // 250 us
        1'000'000ULL,    // 1 ms
        5'000'000ULL,    // 5 ms
        25'000'000ULL,   // 25 ms
        100'000'000ULL,  // 100 ms
        150'000'000ULL,  // 150 ms  <- umbral RNF-004 extremo a extremo
        500'000'000ULL,  // 500 ms
        1'000'000'000ULL // 1 s
    };

    void observe(std::uint64_t ns) noexcept;

    [[nodiscard]] std::uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t sumNs() const noexcept { return sum_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t minNs() const noexcept;
    [[nodiscard]] std::uint64_t maxNs() const noexcept { return max_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t bucket(std::size_t index) const noexcept;

    /// Cuantil estimado por interpolacion sobre los cubos: devuelve el limite
    /// superior del cubo donde cae el cuantil. 0 si no hay muestras.
    [[nodiscard]] std::uint64_t quantileNs(double q) const noexcept;

private:
    std::array<std::atomic<std::uint64_t>, kBucketCount> buckets_{};
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> sum_{0};
    std::atomic<std::uint64_t> min_{UINT64_MAX};
    std::atomic<std::uint64_t> max_{0};
};

/// Mide un ambito y lo registra en un histograma al salir.
class ScopedTimer {
public:
    explicit ScopedTimer(Histogram& hist) noexcept : hist_{hist}, startNs_{monotonicNowNs()} {}
    ScopedTimer(const ScopedTimer&)            = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    ScopedTimer(ScopedTimer&&)                 = delete;
    ScopedTimer& operator=(ScopedTimer&&)      = delete;
    ~ScopedTimer() { hist_.observe(monotonicNowNs() - startNs_); }

private:
    Histogram&    hist_;
    std::uint64_t startNs_;
};

#define PW_TIMED(hist) ::pw::obs::ScopedTimer _pwScopedTimer{hist}

enum class MetricKind : std::uint8_t { Counter, Gauge, Histogram };

/// Lectura de una metrica, para Diagnostics y para el volcado a fichero.
struct MetricSample {
    std::string   name;      ///< p. ej. "udp_packets_invalid_total"
    std::string   labels;    ///< p. ej. "reason=bad_size"; vacio si no tiene
    MetricKind    kind{MetricKind::Counter};
    std::uint64_t count{0};  ///< Counter: valor. Histogram: numero de muestras.
    std::int64_t  gauge{0};  ///< Gauge: valor.
    std::uint64_t sumNs{0};
    std::uint64_t minNs{0};
    std::uint64_t maxNs{0};
    std::uint64_t p50Ns{0};
    std::uint64_t p95Ns{0};
    std::uint64_t p99Ns{0};
};

/// Registro consultable de metricas.
///
/// Registrar es una operacion de arranque y toma un mutex; actualizar no toma
/// ninguno. Las referencias devueltas son estables durante toda la vida del
/// registro, de modo que un componente puede guardarse la suya al construirse.
class MetricsRegistry {
public:
    MetricsRegistry() = default;
    MetricsRegistry(const MetricsRegistry&)            = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;

    [[nodiscard]] Counter&   counter(std::string_view name, std::string_view labels = {});
    [[nodiscard]] Gauge&     gauge(std::string_view name, std::string_view labels = {});
    [[nodiscard]] Histogram& histogram(std::string_view name, std::string_view labels = {});

    /// Instantanea ordenada por nombre y etiquetas (OBS-01, OBS-03).
    [[nodiscard]] std::vector<MetricSample> snapshot() const;

    [[nodiscard]] std::size_t size() const;

private:
    struct Entry {
        std::string name;
        std::string labels;
        MetricKind  kind;
        Counter     counter;
        Gauge       gauge;
        Histogram   histogram;

        Entry(std::string_view n, std::string_view l, MetricKind k) : name{n}, labels{l}, kind{k} {}
    };

    Entry& findOrCreate(std::string_view name, std::string_view labels, MetricKind kind);

    mutable std::mutex mutex_;
    std::deque<Entry>  entries_;  ///< deque: las referencias no se invalidan al crecer
};

} // namespace pw::obs
