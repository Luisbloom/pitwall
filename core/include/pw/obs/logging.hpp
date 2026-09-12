#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::obs — logging estructurado y asincrono (F0-09, RF-804, RF-805, OBS-04..06).
//
// Cinco niveles, escritura en un hilo aparte, rotacion por tamano y agregacion
// de mensajes repetidos. Las tres propiedades responden a requisitos concretos:
//
//   asincrono   T1 no puede bloquearse escribiendo en disco (04-ARCHITECTURE 4.2)
//   rotacion    los logs no pueden crecer sin limite          (RF-805, OBS-05)
//   agregacion  a 415 datagramas/s un mensaje repetido inunda (RNF-102)
//
// El nivel se cambia en caliente, global o por componente (OBS-04, RNF-103):
//
//   [logging]
//   level = "info"
//   overrides = { "pw::state" = "trace", "pw::decoder" = "debug" }
//
// Si la cola se llena, se DESCARTA y se cuenta: registrar jamas puede frenar al
// productor. El recuento de descartes se publica como metrica.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pw::obs {

enum class LogLevel : std::uint8_t { Trace = 0, Debug, Info, Warn, Error, Off };

[[nodiscard]] const char*               logLevelName(LogLevel level) noexcept;
[[nodiscard]] std::optional<LogLevel>   parseLogLevel(std::string_view text) noexcept;

/// Par clave/valor de un registro estructurado (RNF-101).
struct LogField {
    std::string key;
    std::string value;
};

[[nodiscard]] LogField field(std::string_view key, std::string_view value);
[[nodiscard]] LogField field(std::string_view key, std::int64_t value);
[[nodiscard]] LogField field(std::string_view key, std::uint64_t value);
[[nodiscard]] LogField field(std::string_view key, double value);

/// Destino de escritura de una linea ya formateada.
class ILogSink {
public:
    ILogSink()                           = default;
    ILogSink(const ILogSink&)            = delete;
    ILogSink& operator=(const ILogSink&) = delete;
    virtual ~ILogSink()                  = default;

    virtual void write(std::string_view line) = 0;
    virtual void flush()                      = 0;
};

/// Escribe por el error estandar. En compilaciones de desarrollo.
class ConsoleSink final : public ILogSink {
public:
    void write(std::string_view line) override;
    void flush() override;
};

/// Fichero con rotacion por tamano y numero maximo de ficheros (RF-805).
///
/// Al superarse el tamano maximo: pitwall.log -> pitwall.1.log -> pitwall.2.log…
/// y se borra el que exceda maxFiles.
class RotatingFileSink final : public ILogSink {
public:
    RotatingFileSink(std::filesystem::path directory,
                     std::string           baseName,
                     std::uint64_t         maxFileSizeBytes,
                     std::size_t           maxFiles);
    ~RotatingFileSink() override;

    void write(std::string_view line) override;
    void flush() override;

    [[nodiscard]] std::filesystem::path currentPath() const;
    [[nodiscard]] std::size_t           rotations() const noexcept { return rotations_; }

private:
    void open();
    void rotate();

    std::filesystem::path directory_;
    std::string           baseName_;
    std::uint64_t         maxFileSizeBytes_;
    std::size_t           maxFiles_;
    std::uint64_t         currentSize_{0};
    std::size_t           rotations_{0};
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct LoggerConfig {
    LogLevel                                     level{LogLevel::Info};
    std::unordered_map<std::string, LogLevel>    componentOverrides{};
    std::size_t                                  queueCapacity{4096};
    /// Ventana de agregacion de mensajes identicos (RNF-102). 0 la desactiva.
    std::uint64_t                                aggregationWindowMs{1000};
};

struct LoggerStats {
    std::uint64_t emitted{0};    ///< Lineas escritas en los destinos.
    std::uint64_t dropped{0};    ///< Registros perdidos por cola llena.
    std::uint64_t aggregated{0}; ///< Registros absorbidos por un repetido.
};

/// Registrador asincrono. Construirlo arranca su hilo; destruirlo lo drena.
class Logger {
public:
    explicit Logger(LoggerConfig config);
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;
    ~Logger();

    void addSink(std::shared_ptr<ILogSink> sink);

    // ── Nivel, en caliente (OBS-04, RNF-103) ─────────────────────────────────
    void                    setLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel  level() const noexcept;
    void                    setComponentLevel(std::string_view component, LogLevel level);
    void                    clearComponentLevels();
    [[nodiscard]] LogLevel  levelFor(std::string_view component) const;
    [[nodiscard]] bool      enabled(std::string_view component, LogLevel level) const;

    // ── Emision ──────────────────────────────────────────────────────────────
    void log(LogLevel level, std::string_view component, std::string_view message,
             std::vector<LogField> fields = {});

    void trace(std::string_view component, std::string_view message, std::vector<LogField> f = {});
    void debug(std::string_view component, std::string_view message, std::vector<LogField> f = {});
    void info (std::string_view component, std::string_view message, std::vector<LogField> f = {});
    void warn (std::string_view component, std::string_view message, std::vector<LogField> f = {});
    void error(std::string_view component, std::string_view message, std::vector<LogField> f = {});

    /// Bloquea hasta que la cola esta vacia y los destinos vaciados. Para
    /// pruebas y para el cierre ordenado (RF-629).
    void flush();

    [[nodiscard]] LoggerStats stats() const noexcept;

private:
    struct Record {
        LogLevel              level{LogLevel::Info};
        std::string           component;
        std::string           message;
        std::vector<LogField> fields;
        std::int64_t          wallMs{0};
        std::uint64_t         monotonicNs{0};
    };

    struct Pending {
        Record        record;
        std::uint64_t repeats{0};
        std::uint64_t lastEmitNs{0};
    };

    void run();
    void emit(const Record& record, std::uint64_t repeats);
    void drainAggregates(std::uint64_t nowNs, bool force);

    LoggerConfig                            config_;
    std::atomic<LogLevel>                   level_;
    mutable std::mutex                      overridesMutex_;
    std::unordered_map<std::string, LogLevel> overrides_;

    mutable std::mutex                      queueMutex_;
    std::condition_variable                 queueCv_;
    std::condition_variable                 drainedCv_;
    std::deque<Record>                      queue_;
    bool                                    stopping_{false};
    bool                                    busy_{false};

    std::mutex                              sinksMutex_;
    std::vector<std::shared_ptr<ILogSink>>  sinks_;

    std::unordered_map<std::string, Pending> aggregates_;

    std::atomic<std::uint64_t>              emitted_{0};
    std::atomic<std::uint64_t>              dropped_{0};
    std::atomic<std::uint64_t>              aggregated_{0};

    std::thread                             worker_;
};

} // namespace pw::obs
