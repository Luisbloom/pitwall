#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::config — configuracion de PitWall (F0-10).
//
// El esquema es el de 09-BACKEND-PLAN.md §6, literalmente. La precedencia,
// de menor a mayor:
//
//     valores por defecto  ->  fichero TOML  ->  variables de entorno  ->  argumentos
//
// Toda la configuracion se valida al arrancar con mensajes que dicen QUE esta
// mal y QUE se esperaba (RNF-097). Un mensaje de error de configuracion que no
// diga como arreglarlo se considera un defecto.
//
// Lo que NO es configurable, por decision de arquitectura:
//   - El transporte interno: siempre 127.0.0.1, puerto efimero, token de un solo
//     uso (ADR-002 v2, SEC-13, SEC-14). Solo se configuran sus ritmos.
//   - Cualquier forma de exponer la interfaz a la red (OUT-11).
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pw/core/result.hpp"

namespace pw::config {

/// Origen de los datagramas: el juego en vivo o una captura.
enum class Mode : std::uint8_t { Live, Replay };

/// Velocidad de reproduccion. 0 significa «tan rapido como se pueda» (RF-406).
inline constexpr double kSpeedMax = 0.0;

// ── Dimensionado del pool de buffers ────────────────────────────────────────
//
// El minimo NO lo fija la rafaga de 10 s (415 x 10 = 4.150), sino el modelo de
// propiedad: Qraw y Qrec mantienen cada una su referencia sobre el buffer, y dos
// colas que avanzan a ritmos distintos acaban refiriendose a conjuntos
// DISJUNTOS. Por debajo de la suma, el pool se agota antes de que ninguna cola
// se llene y las politicas de desbordamiento por cola no llegan a aplicarse:
// un grabador atascado deja sin buffers al pipeline y se pierde todo.
//
//     8192 (Qraw) + 8192 (Qrec) + 2 (uno en la mano de cada consumidor)
//                               + 1 (el que el productor esta rellenando)   = 16.387
//
// La deduccion vive en pw::mem::poolSizeForQueues y el numero en
// pw::mem::kMinimumPoolSize. Aqui se repite porque pw::config NO puede depender
// de pw::mem (grafo de modulos, RNF-084); una prueba verifica que los dos
// valores coinciden, de modo que no puedan separarse en silencio.
inline constexpr std::uint32_t kMinimumPoolSize = 16'387;

/// Valor por defecto: el minimo redondeado al alza con margen, y multiplo de
/// 1024. Son ~49 s de rafaga a 415 datagramas/s y 41 MiB de memoria reservada
/// con buffers de 2048 B, muy por debajo del presupuesto de RNF-011.
inline constexpr std::uint32_t kDefaultPoolSize = 20'480;

static_assert(kDefaultPoolSize >= kMinimumPoolSize,
              "El valor por defecto del pool debe cumplir su propio minimo: "
              "es la discrepancia que se cerro en F1.1-01");

struct NetConfig {
    std::string              bind{"127.0.0.1"};
    std::uint16_t            port{20777};
    std::uint32_t            receiveBufferBytes{4 * 1024 * 1024};
    std::uint32_t            poolSize{kDefaultPoolSize};
    std::vector<std::string> originFilter{};   ///< vacio = aceptar cualquier origen
};

struct RecordingConfig {
    bool          enabled{true};
    bool          autoStartOnSession{true};
    std::string   directory{"%LOCALAPPDATA%/PitWall/data/captures"};
    std::uint32_t compressionLevel{3};
    std::uint64_t maxFileSizeBytes{1024ULL * 1024ULL * 1024ULL};
    std::uint32_t syncPointIntervalSeconds{10};
};

struct StorageConfig {
    std::string   catalogPath{"%LOCALAPPDATA%/PitWall/data/catalog.db"};
    std::string   seriesPath{"%LOCALAPPDATA%/PitWall/data/series"};
    std::uint32_t retentionRawGb{20};
    std::uint32_t retentionSeriesGb{50};
    std::uint32_t retentionDays{90};
};

struct TransportConfig {
    std::uint32_t timingRateHz{10};
    std::uint32_t telemetryRateHz{20};
};

struct ShellConfig {
    bool        startMaximized{false};
    bool        rememberWindowState{true};
    bool        alwaysOnTop{false};
    bool        devTools{false};
    std::string uiSource{"embedded"};                   ///< "embedded" | "dev-server"
    std::string devServerUrl{"http://localhost:5173"};
};

struct LoggingConfig {
    std::string                        level{"info"};
    std::string                        directory{"%LOCALAPPDATA%/PitWall/data/logs"};
    std::uint32_t                      maxFileSizeMb{32};
    std::uint32_t                      maxFiles{10};
    std::map<std::string, std::string> overrides{};     ///< componente -> nivel
    bool                               console{false};  ///< eco por consola (desarrollo)
};

struct AnalysisConfig {
    double        distanceStepMeters{1.0};
    double        brakingThreshold{0.15};
    std::uint32_t minCleanLapsForDegradation{5};
    double        maxInterpolationGapMeters{25.0};
};

struct ReplayConfig {
    std::string path{};
    double      speed{1.0};
};

struct Config {
    Mode            mode{Mode::Live};
    bool            headless{false};   ///< nucleo sin interfaz (pitwall-cli, pruebas)
    NetConfig       net{};
    RecordingConfig recording{};
    StorageConfig   storage{};
    TransportConfig transport{};
    ShellConfig     shell{};
    LoggingConfig   logging{};
    AnalysisConfig  analysis{};
    ReplayConfig    replay{};

    /// Fichero del que se cargo, si lo hubo. Vacio si solo hay defectos.
    std::filesystem::path sourceFile{};
};

/// Clasificacion del fallo, para que quien llama decida como reaccionar.
enum class ConfigErrorKind : std::uint8_t {
    FileNotFound,
    ParseError,
    InvalidValue,
    UnknownKey,
    UnknownArgument,
    HelpRequested,
    VersionRequested
};

struct ConfigError {
    ConfigErrorKind kind{ConfigErrorKind::InvalidValue};
    std::string     key{};       ///< p. ej. "net.port"; vacio si no aplica
    std::string     found{};     ///< valor recibido, tal cual
    std::string     expected{};  ///< que se esperaba, en lenguaje llano
    std::string     message{};   ///< frase completa y accionable

    [[nodiscard]] std::string describe() const;
};

/// Consulta de variables de entorno. Inyectable para poder probarla.
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

/// Lector que consulta el entorno real del proceso.
[[nodiscard]] EnvLookup systemEnv();

/// Configuracion con todos los valores por defecto documentados.
[[nodiscard]] Config defaults();

/// Aplica un fichero TOML sobre una configuracion base.
[[nodiscard]] Result<Config, ConfigError> applyFile(Config base,
                                                    const std::filesystem::path& path);

/// Aplica un fichero TOML ya leido en memoria (para pruebas y para el
/// diagnostico de errores sin tocar disco).
[[nodiscard]] Result<Config, ConfigError> applyToml(Config base, std::string_view toml,
                                                    std::string_view originName = "<memoria>");

/// Aplica las variables de entorno PITWALL_*.
[[nodiscard]] Result<Config, ConfigError> applyEnvironment(Config base, const EnvLookup& env);

/// Aplica los argumentos de linea de ordenes.
[[nodiscard]] Result<Config, ConfigError> applyCommandLine(Config base,
                                                           const std::vector<std::string>& args);

/// Precedencia completa: defectos -> fichero -> entorno -> argumentos -> validacion.
/// `args` NO incluye argv[0].
[[nodiscard]] Result<Config, ConfigError> load(const std::vector<std::string>& args,
                                               const EnvLookup&                env);

/// Validacion independiente, tambien util tras modificar la configuracion.
[[nodiscard]] Result<void, ConfigError> validate(const Config& config);

/// Asigna una clave con notacion de punto ("net.port") desde texto. Es el camino
/// que comparten las variables de entorno y los argumentos.
[[nodiscard]] Result<void, ConfigError> setValue(Config& config, std::string_view dottedKey,
                                                 std::string_view rawValue);

/// Expande %VAR% y $VAR en una ruta. %LOCALAPPDATA% tiene equivalente en Linux
/// para que el nucleo siga siendo multiplataforma (RNF-091).
[[nodiscard]] std::filesystem::path expandPath(std::string_view raw, const EnvLookup& env);

/// Texto de ayuda de la linea de ordenes.
[[nodiscard]] std::string usageText();

/// Claves aceptadas por setValue, con su variable de entorno equivalente.
struct KeyBinding {
    std::string_view dottedKey;
    std::string_view environmentVariable;
    std::string_view description;
};
[[nodiscard]] std::vector<KeyBinding> keyBindings();

} // namespace pw::config
