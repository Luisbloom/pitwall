#include "pw/config/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#include <toml++/toml.hpp>

#include "pw/obs/logging.hpp"

namespace pw::config {
namespace {

// ── Utilidades de error ──────────────────────────────────────────────────────

ConfigError makeError(const ConfigErrorKind kind, std::string key, std::string found,
                      std::string expected)
{
    ConfigError error;
    error.kind     = kind;
    error.key      = std::move(key);
    error.found    = std::move(found);
    error.expected = std::move(expected);

    std::ostringstream out;
    switch (kind) {
    case ConfigErrorKind::FileNotFound:
        out << "no se encuentra el fichero de configuracion '" << error.found
            << "'. " << error.expected;
        break;
    case ConfigErrorKind::ParseError:
        out << "el fichero de configuracion '" << error.key << "' no es TOML valido: "
            << error.found << ". " << error.expected;
        break;
    case ConfigErrorKind::UnknownKey:
        out << "clave de configuracion desconocida '" << error.key << "'. " << error.expected;
        break;
    case ConfigErrorKind::UnknownArgument:
        out << "argumento desconocido '" << error.found << "'. " << error.expected;
        break;
    case ConfigErrorKind::HelpRequested:
    case ConfigErrorKind::VersionRequested:
        out << error.expected;
        break;
    case ConfigErrorKind::InvalidValue:
        out << "valor invalido en '" << error.key << "': se recibio '" << error.found
            << "' y se esperaba " << error.expected;
        break;
    }
    error.message = out.str();
    return error;
}

Unexpected<ConfigError> invalid(const std::string_view key, const std::string_view found,
                                const std::string_view expected)
{
    return Unexpected<ConfigError>{makeError(ConfigErrorKind::InvalidValue, std::string{key},
                                             std::string{found}, std::string{expected})};
}

// ── Conversiones con mensaje accionable ──────────────────────────────────────

Result<bool, ConfigError> parseBool(const std::string_view key, const std::string_view raw)
{
    if (raw == "true" || raw == "1" || raw == "on" || raw == "yes") { return true; }
    if (raw == "false" || raw == "0" || raw == "off" || raw == "no") { return false; }
    return invalid(key, raw, "true o false");
}

Result<std::uint64_t, ConfigError> parseUnsigned(const std::string_view key,
                                                 const std::string_view raw,
                                                 const std::uint64_t    minValue,
                                                 const std::uint64_t    maxValue)
{
    std::string text{raw};
    std::size_t consumed = 0;
    std::uint64_t value  = 0;
    try {
        value = std::stoull(text, &consumed, 10);
    } catch (const std::exception&) {
        return invalid(key, raw,
                       "un entero entre " + std::to_string(minValue) + " y " +
                           std::to_string(maxValue));
    }
    if (consumed != text.size()) {
        return invalid(key, raw,
                       "un entero entre " + std::to_string(minValue) + " y " +
                           std::to_string(maxValue));
    }
    if (value < minValue || value > maxValue) {
        return invalid(key, raw,
                       "un entero entre " + std::to_string(minValue) + " y " +
                           std::to_string(maxValue));
    }
    return value;
}

Result<double, ConfigError> parseDouble(const std::string_view key, const std::string_view raw,
                                        const double minValue, const double maxValue)
{
    std::string text{raw};
    std::size_t consumed = 0;
    double      value    = 0.0;
    try {
        value = std::stod(text, &consumed);
    } catch (const std::exception&) {
        return invalid(key, raw, "un numero decimal");
    }
    if (consumed != text.size()) { return invalid(key, raw, "un numero decimal"); }
    if (value < minValue || value > maxValue) {
        std::ostringstream out;
        out << "un numero entre " << minValue << " y " << maxValue;
        return invalid(key, raw, out.str());
    }
    return value;
}

std::vector<std::string> splitCsv(const std::string_view raw)
{
    std::vector<std::string> out;
    std::string              current;
    for (const char c : raw) {
        if (c == ',') {
            if (!current.empty()) { out.push_back(current); }
            current.clear();
        } else if (c != ' ') {
            current.push_back(c);
        }
    }
    if (!current.empty()) { out.push_back(current); }
    return out;
}

/// Representacion textual de un nodo TOML, para reutilizar setValue y tener un
/// unico sitio donde se validan los valores.
std::optional<std::string> nodeToString(const toml::node& node)
{
    if (const auto* v = node.as_string())  { return v->get(); }
    if (const auto* v = node.as_integer()) { return std::to_string(v->get()); }
    if (const auto* v = node.as_boolean()) { return v->get() ? "true" : "false"; }
    if (const auto* v = node.as_floating_point()) {
        std::ostringstream out;
        out << v->get();
        return out.str();
    }
    return std::nullopt;
}

} // namespace

// ── ConfigError ──────────────────────────────────────────────────────────────

std::string ConfigError::describe() const
{
    return message;
}

// ── Entorno y rutas ──────────────────────────────────────────────────────────

EnvLookup systemEnv()
{
    return [](const std::string_view name) -> std::optional<std::string> {
        const std::string key{name};
#if defined(_MSC_VER)
        char*       buffer = nullptr;
        std::size_t size   = 0;
        if (_dupenv_s(&buffer, &size, key.c_str()) != 0 || buffer == nullptr) {
            return std::nullopt;
        }
        std::string value{buffer};
        std::free(buffer);
        if (value.empty()) { return std::nullopt; }
        return value;
#else
        const char* value = std::getenv(key.c_str());
        if (value == nullptr || *value == '\0') { return std::nullopt; }
        return std::string{value};
#endif
    };
}

std::filesystem::path expandPath(const std::string_view raw, const EnvLookup& env)
{
    std::string out;
    out.reserve(raw.size());

    const auto lookup = [&env](const std::string& name) -> std::string {
        if (auto value = env(name)) { return *value; }
        // El nucleo compila y se ejecuta tambien en Linux (RNF-091), donde las
        // variables de Windows no existen: se traducen a su equivalente.
        if (name == "LOCALAPPDATA" || name == "APPDATA") {
            if (auto home = env("HOME")) { return *home + "/.local/share"; }
            return ".";
        }
        if (name == "USERPROFILE") {
            if (auto home = env("HOME")) { return *home; }
            return ".";
        }
        return {};
    };

    for (std::size_t i = 0; i < raw.size();) {
        if (raw[i] == '%') {
            const auto end = raw.find('%', i + 1);
            if (end != std::string_view::npos) {
                out += lookup(std::string{raw.substr(i + 1, end - i - 1)});
                i = end + 1;
                continue;
            }
        }
        if (raw[i] == '$' && i + 1 < raw.size() && (std::isalpha(static_cast<unsigned char>(raw[i + 1])) != 0 || raw[i + 1] == '_')) {
            std::size_t end = i + 1;
            while (end < raw.size() &&
                   (std::isalnum(static_cast<unsigned char>(raw[end])) != 0 || raw[end] == '_')) {
                ++end;
            }
            out += lookup(std::string{raw.substr(i + 1, end - i - 1)});
            i = end;
            continue;
        }
        out.push_back(raw[i]);
        ++i;
    }
    return std::filesystem::path{out}.lexically_normal();
}

// ── Claves ───────────────────────────────────────────────────────────────────

std::vector<KeyBinding> keyBindings()
{
    return {
        {"net.bind",                          "PITWALL_NET_BIND",                          "Direccion de escucha UDP"},
        {"net.port",                          "PITWALL_NET_PORT",                          "Puerto UDP del juego"},
        {"net.receiveBufferBytes",            "PITWALL_NET_RECEIVEBUFFERBYTES",            "Tamano de SO_RCVBUF"},
        {"net.poolSize",                      "PITWALL_NET_POOLSIZE",                      "Numero de buffers preasignados"},
        {"net.originFilter",                  "PITWALL_NET_ORIGINFILTER",                  "Origenes aceptados, separados por comas"},
        {"net.silenceTimeoutMs",              "PITWALL_NET_SILENCETIMEOUTMS",              "Silencio que marca la fuente UDP como inactiva (RF-012)"},
        {"recording.enabled",                 "PITWALL_RECORDING_ENABLED",                 "Grabacion cruda activada"},
        {"recording.autoStartOnSession",      "PITWALL_RECORDING_AUTOSTARTONSESSION",      "Empezar a grabar al detectar sesion"},
        {"recording.directory",               "PITWALL_RECORDING_DIRECTORY",               "Carpeta de capturas"},
        {"recording.compressionLevel",        "PITWALL_RECORDING_COMPRESSIONLEVEL",        "Nivel de compresion zstd"},
        {"recording.maxFileSizeBytes",        "PITWALL_RECORDING_MAXFILESIZEBYTES",        "Tamano maximo por captura"},
        {"recording.syncPointIntervalSeconds","PITWALL_RECORDING_SYNCPOINTINTERVALSECONDS","Intervalo de puntos de sincronizacion"},
        {"storage.catalogPath",               "PITWALL_STORAGE_CATALOGPATH",               "Ruta del catalogo"},
        {"storage.seriesPath",                "PITWALL_STORAGE_SERIESPATH",                "Ruta de las series"},
        {"storage.retentionRawGb",            "PITWALL_STORAGE_RETENTIONRAWGB",            "Retencion de capturas en GB"},
        {"storage.retentionSeriesGb",         "PITWALL_STORAGE_RETENTIONSERIESGB",         "Retencion de series en GB"},
        {"storage.retentionDays",             "PITWALL_STORAGE_RETENTIONDAYS",             "Retencion en dias"},
        {"transport.timingRateHz",            "PITWALL_TRANSPORT_TIMINGRATEHZ",            "Ritmo del canal timing"},
        {"transport.telemetryRateHz",         "PITWALL_TRANSPORT_TELEMETRYRATEHZ",         "Ritmo del canal telemetry"},
        {"shell.startMaximized",              "PITWALL_SHELL_STARTMAXIMIZED",              "Abrir la ventana maximizada"},
        {"shell.rememberWindowState",         "PITWALL_SHELL_REMEMBERWINDOWSTATE",         "Recordar posicion y tamano"},
        {"shell.alwaysOnTop",                 "PITWALL_SHELL_ALWAYSONTOP",                 "Ventana siempre encima"},
        {"shell.devTools",                    "PITWALL_SHELL_DEVTOOLS",                    "Herramientas de desarrollo del WebView2"},
        {"shell.uiSource",                    "PITWALL_SHELL_UISOURCE",                    "embedded o dev-server"},
        {"shell.devServerUrl",                "PITWALL_SHELL_DEVSERVERURL",                "URL del servidor de desarrollo"},
        {"logging.level",                     "PITWALL_LOGGING_LEVEL",                     "trace, debug, info, warn, error u off"},
        {"logging.directory",                 "PITWALL_LOGGING_DIRECTORY",                 "Carpeta de logs"},
        {"logging.maxFileSizeMb",             "PITWALL_LOGGING_MAXFILESIZEMB",             "Tamano maximo por fichero de log"},
        {"logging.maxFiles",                  "PITWALL_LOGGING_MAXFILES",                  "Numero de logs conservados"},
        {"logging.console",                   "PITWALL_LOGGING_CONSOLE",                   "Eco de los logs por consola"},
        {"analysis.distanceStepMeters",       "PITWALL_ANALYSIS_DISTANCESTEPMETERS",       "Paso de remuestreo por distancia"},
        {"analysis.brakingThreshold",         "PITWALL_ANALYSIS_BRAKINGTHRESHOLD",         "Umbral de deteccion de frenada"},
        {"analysis.minCleanLapsForDegradation","PITWALL_ANALYSIS_MINCLEANLAPSFORDEGRADATION","Vueltas limpias minimas"},
        {"analysis.maxInterpolationGapMeters","PITWALL_ANALYSIS_MAXINTERPOLATIONGAPMETERS","Hueco maximo interpolable"},
        {"replay.path",                       "PITWALL_REPLAY_PATH",                       "Captura a reproducir"},
        {"replay.speed",                      "PITWALL_REPLAY_SPEED",                      "Velocidad de reproduccion"},
    };
}

// ── setValue ─────────────────────────────────────────────────────────────────

Result<void, ConfigError> setValue(Config& config, const std::string_view dottedKey,
                                   const std::string_view rawValue)
{
    const auto assignBool = [&](bool& target) -> Result<void, ConfigError> {
        auto parsed = parseBool(dottedKey, rawValue);
        if (!parsed) { return Unexpected<ConfigError>{parsed.error()}; }
        target = parsed.value();
        return {};
    };
    const auto assignUnsigned = [&](auto& target, std::uint64_t lo,
                                    std::uint64_t hi) -> Result<void, ConfigError> {
        auto parsed = parseUnsigned(dottedKey, rawValue, lo, hi);
        if (!parsed) { return Unexpected<ConfigError>{parsed.error()}; }
        target = static_cast<std::remove_reference_t<decltype(target)>>(parsed.value());
        return {};
    };
    const auto assignDouble = [&](double& target, double lo, double hi) -> Result<void, ConfigError> {
        auto parsed = parseDouble(dottedKey, rawValue, lo, hi);
        if (!parsed) { return Unexpected<ConfigError>{parsed.error()}; }
        target = parsed.value();
        return {};
    };

    // Niveles por componente: logging.overrides.<componente>
    constexpr std::string_view kOverridePrefix = "logging.overrides.";
    if (dottedKey.rfind(kOverridePrefix, 0) == 0) {
        const auto component = std::string{dottedKey.substr(kOverridePrefix.size())};
        if (component.empty()) {
            return invalid(dottedKey, rawValue, "un nombre de componente, p. ej. logging.overrides.\"pw::state\"");
        }
        if (!obs::parseLogLevel(rawValue).has_value()) {
            return invalid(dottedKey, rawValue, "trace, debug, info, warn, error u off");
        }
        config.logging.overrides[component] = std::string{rawValue};
        return {};
    }

    if (dottedKey == "net.bind") {
        if (rawValue.empty()) { return invalid(dottedKey, rawValue, "una direccion IPv4, p. ej. 127.0.0.1 o 0.0.0.0"); }
        config.net.bind = rawValue;
        return {};
    }
    if (dottedKey == "net.port")               { return assignUnsigned(config.net.port, 1, 65535); }
    if (dottedKey == "net.receiveBufferBytes") { return assignUnsigned(config.net.receiveBufferBytes, 65536, 256ULL * 1024ULL * 1024ULL); }
    if (dottedKey == "net.poolSize")           { return assignUnsigned(config.net.poolSize, 64, 1'048'576); }
    if (dottedKey == "net.silenceTimeoutMs") {
        return assignUnsigned(config.net.silenceTimeoutMs, kMinSilenceTimeoutMs, kMaxSilenceTimeoutMs);
    }
    if (dottedKey == "net.originFilter") {
        config.net.originFilter = splitCsv(rawValue);
        return {};
    }

    if (dottedKey == "recording.enabled")            { return assignBool(config.recording.enabled); }
    if (dottedKey == "recording.autoStartOnSession") { return assignBool(config.recording.autoStartOnSession); }
    if (dottedKey == "recording.directory") {
        if (rawValue.empty()) { return invalid(dottedKey, rawValue, "una ruta de carpeta no vacia"); }
        config.recording.directory = rawValue;
        return {};
    }
    if (dottedKey == "recording.compressionLevel")         { return assignUnsigned(config.recording.compressionLevel, 1, 19); }
    if (dottedKey == "recording.maxFileSizeBytes")         { return assignUnsigned(config.recording.maxFileSizeBytes, 1024ULL * 1024ULL, 1024ULL * 1024ULL * 1024ULL * 64ULL); }
    if (dottedKey == "recording.syncPointIntervalSeconds") { return assignUnsigned(config.recording.syncPointIntervalSeconds, 1, 3600); }

    if (dottedKey == "storage.catalogPath") {
        if (rawValue.empty()) { return invalid(dottedKey, rawValue, "una ruta de fichero no vacia"); }
        config.storage.catalogPath = rawValue;
        return {};
    }
    if (dottedKey == "storage.seriesPath") {
        if (rawValue.empty()) { return invalid(dottedKey, rawValue, "una ruta de carpeta no vacia"); }
        config.storage.seriesPath = rawValue;
        return {};
    }
    if (dottedKey == "storage.retentionRawGb")    { return assignUnsigned(config.storage.retentionRawGb, 1, 100'000); }
    if (dottedKey == "storage.retentionSeriesGb") { return assignUnsigned(config.storage.retentionSeriesGb, 1, 100'000); }
    if (dottedKey == "storage.retentionDays")     { return assignUnsigned(config.storage.retentionDays, 1, 36'500); }

    if (dottedKey == "transport.timingRateHz")    { return assignUnsigned(config.transport.timingRateHz, 1, 60); }
    if (dottedKey == "transport.telemetryRateHz") { return assignUnsigned(config.transport.telemetryRateHz, 1, 120); }

    if (dottedKey == "shell.startMaximized")      { return assignBool(config.shell.startMaximized); }
    if (dottedKey == "shell.rememberWindowState") { return assignBool(config.shell.rememberWindowState); }
    if (dottedKey == "shell.alwaysOnTop")         { return assignBool(config.shell.alwaysOnTop); }
    if (dottedKey == "shell.devTools")            { return assignBool(config.shell.devTools); }
    if (dottedKey == "shell.uiSource") {
        if (rawValue != "embedded" && rawValue != "dev-server") {
            return invalid(dottedKey, rawValue, "embedded o dev-server");
        }
        config.shell.uiSource = rawValue;
        return {};
    }
    if (dottedKey == "shell.devServerUrl") {
        if (rawValue.rfind("http://", 0) != 0 && rawValue.rfind("https://", 0) != 0) {
            return invalid(dottedKey, rawValue, "una URL http:// o https://, p. ej. http://localhost:5173");
        }
        config.shell.devServerUrl = rawValue;
        return {};
    }

    if (dottedKey == "logging.level") {
        if (!obs::parseLogLevel(rawValue).has_value()) {
            return invalid(dottedKey, rawValue, "trace, debug, info, warn, error u off");
        }
        config.logging.level = rawValue;
        return {};
    }
    if (dottedKey == "logging.directory") {
        if (rawValue.empty()) { return invalid(dottedKey, rawValue, "una ruta de carpeta no vacia"); }
        config.logging.directory = rawValue;
        return {};
    }
    if (dottedKey == "logging.maxFileSizeMb") { return assignUnsigned(config.logging.maxFileSizeMb, 1, 4096); }
    if (dottedKey == "logging.maxFiles")      { return assignUnsigned(config.logging.maxFiles, 1, 1000); }
    if (dottedKey == "logging.console")       { return assignBool(config.logging.console); }

    if (dottedKey == "analysis.distanceStepMeters")        { return assignDouble(config.analysis.distanceStepMeters, 0.01, 100.0); }
    if (dottedKey == "analysis.brakingThreshold")          { return assignDouble(config.analysis.brakingThreshold, 0.0, 1.0); }
    if (dottedKey == "analysis.minCleanLapsForDegradation"){ return assignUnsigned(config.analysis.minCleanLapsForDegradation, 1, 1000); }
    if (dottedKey == "analysis.maxInterpolationGapMeters") { return assignDouble(config.analysis.maxInterpolationGapMeters, 0.0, 10'000.0); }

    if (dottedKey == "replay.path") {
        config.replay.path = rawValue;
        config.mode        = rawValue.empty() ? Mode::Live : Mode::Replay;
        return {};
    }
    if (dottedKey == "replay.speed") {
        if (rawValue == "max") {
            config.replay.speed = kSpeedMax;
            return {};
        }
        auto text = std::string{rawValue};
        if (!text.empty() && (text.back() == 'x' || text.back() == 'X')) { text.pop_back(); }
        auto parsed = parseDouble(dottedKey, text, 0.01, 64.0);
        if (!parsed) {
            return invalid(dottedKey, rawValue, "0.25x, 0.5x, 1x, 2x, 4x o max");
        }
        config.replay.speed = parsed.value();
        return {};
    }

    return Unexpected<ConfigError>{makeError(
        ConfigErrorKind::UnknownKey, std::string{dottedKey}, std::string{rawValue},
        "revise la lista de claves de config/default.toml; las secciones validas son "
        "net, recording, storage, transport, shell, logging, analysis y replay")};
}

// ── Defectos ─────────────────────────────────────────────────────────────────

Config defaults()
{
    return Config{};
}

// ── Fichero ──────────────────────────────────────────────────────────────────

Result<Config, ConfigError> applyToml(Config base, const std::string_view toml,
                                      const std::string_view originName)
{
    toml::table table;
    try {
        table = toml::parse(toml, std::string{originName});
    } catch (const toml::parse_error& error) {
        std::ostringstream detail;
        detail << error.description() << " (linea " << error.source().begin.line << ", columna "
               << error.source().begin.column << ")";
        return Unexpected<ConfigError>{makeError(ConfigErrorKind::ParseError,
                                                 std::string{originName}, detail.str(),
                                                 "corrija la sintaxis TOML en esa posicion")};
    }

    for (const auto& [sectionKey, sectionNode] : table) {
        const auto* section = sectionNode.as_table();
        if (section == nullptr) {
            return Unexpected<ConfigError>{makeError(
                ConfigErrorKind::UnknownKey, std::string{sectionKey.str()}, "",
                "las claves de primer nivel deben ser secciones, p. ej. [net] o [logging]")};
        }
        for (const auto& [entryKey, entryNode] : *section) {
            const auto dotted = std::string{sectionKey.str()} + "." + std::string{entryKey.str()};

            // Tabla anidada: hoy solo logging.overrides.
            if (const auto* nested = entryNode.as_table(); nested != nullptr) {
                for (const auto& [nestedKey, nestedNode] : *nested) {
                    const auto nestedDotted = dotted + "." + std::string{nestedKey.str()};
                    const auto text         = nodeToString(nestedNode);
                    if (!text) {
                        return invalid(nestedDotted, "<no textual>", "un valor simple");
                    }
                    if (auto applied = setValue(base, nestedDotted, *text); !applied) {
                        return Unexpected<ConfigError>{applied.error()};
                    }
                }
                continue;
            }

            // Array: hoy solo net.originFilter.
            if (const auto* array = entryNode.as_array(); array != nullptr) {
                std::string joined;
                for (const auto& element : *array) {
                    const auto text = nodeToString(element);
                    if (!text) { return invalid(dotted, "<no textual>", "una lista de textos"); }
                    if (!joined.empty()) { joined += ','; }
                    joined += *text;
                }
                if (auto applied = setValue(base, dotted, joined); !applied) {
                    return Unexpected<ConfigError>{applied.error()};
                }
                continue;
            }

            const auto text = nodeToString(entryNode);
            if (!text) { return invalid(dotted, "<no textual>", "un valor simple"); }
            if (auto applied = setValue(base, dotted, *text); !applied) {
                return Unexpected<ConfigError>{applied.error()};
            }
        }
    }
    return base;
}

Result<Config, ConfigError> applyFile(Config base, const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Unexpected<ConfigError>{makeError(
            ConfigErrorKind::FileNotFound, "config", path.string(),
            "compruebe la ruta pasada en --config, o use config/default.toml del repositorio")};
    }

    std::ifstream stream{path, std::ios::in | std::ios::binary};
    if (!stream) {
        return Unexpected<ConfigError>{makeError(
            ConfigErrorKind::FileNotFound, "config", path.string(),
            "el fichero existe pero no se puede leer: compruebe los permisos")};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    auto applied = applyToml(std::move(base), buffer.str(), path.string());
    if (!applied) { return applied; }

    auto config       = applied.value();
    config.sourceFile = path;
    return config;
}

// ── Entorno ──────────────────────────────────────────────────────────────────

Result<Config, ConfigError> applyEnvironment(Config base, const EnvLookup& env)
{
    if (!env) { return base; }
    for (const auto& binding : keyBindings()) {
        if (auto value = env(binding.environmentVariable)) {
            if (auto applied = setValue(base, binding.dottedKey, *value); !applied) {
                auto error = applied.error();
                error.message += " (procedente de la variable de entorno " +
                                 std::string{binding.environmentVariable} + ")";
                return Unexpected<ConfigError>{error};
            }
        }
    }
    return base;
}

// ── Linea de ordenes ─────────────────────────────────────────────────────────

std::string usageText()
{
    return
        "PitWall — telemetria e ingenieria de carrera para EA SPORTS F1 25\n"
        "\n"
        "Uso: pitwall-cli [opciones]\n"
        "\n"
        "  --config <ruta>       Fichero TOML de configuracion\n"
        "  --replay <ruta>       Reproducir una captura .f1raw en lugar de escuchar UDP\n"
        "  --speed <v>           Velocidad de replay: 0.25x, 0.5x, 1x, 2x, 4x o max\n"
        "  --port <n>            Puerto UDP de escucha (por defecto 20777)\n"
        "  --bind <direccion>    Direccion de escucha (por defecto 127.0.0.1)\n"
        "  --log-level <nivel>   trace, debug, info, warn, error u off\n"
        "  --log-console         Ademas del fichero, escribir los logs por consola\n"
        "  --record              Forzar la grabacion cruda\n"
        "  --no-record           Desactivar la grabacion cruda\n"
        "  --headless            Nucleo sin interfaz\n"
        "  --set <clave=valor>   Fijar cualquier clave, p. ej. --set net.poolSize=8192\n"
        "  -h, --help            Mostrar esta ayuda\n"
        "  -v, --version         Mostrar la version\n";
}

Result<Config, ConfigError> applyCommandLine(Config base, const std::vector<std::string>& args)
{
    const auto needsValue = [&](const std::size_t index,
                                const std::string& flag) -> Result<std::string, ConfigError> {
        if (index + 1 >= args.size()) {
            return Unexpected<ConfigError>{makeError(
                ConfigErrorKind::InvalidValue, flag, "<vacio>",
                "un valor a continuacion del argumento")};
        }
        return args[index + 1];
    };

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];

        if (arg == "-h" || arg == "--help") {
            return Unexpected<ConfigError>{
                makeError(ConfigErrorKind::HelpRequested, "", "", usageText())};
        }
        if (arg == "-v" || arg == "--version") {
            return Unexpected<ConfigError>{
                makeError(ConfigErrorKind::VersionRequested, "", "", "PitWall 0.1.0")};
        }
        if (arg == "--headless")     { base.headless = true; continue; }
        if (arg == "--record")       { base.recording.enabled = true; continue; }
        if (arg == "--no-record")    { base.recording.enabled = false; continue; }
        if (arg == "--log-console")  { base.logging.console = true; continue; }

        const auto applySimple = [&](const std::string_view key) -> Result<Config, ConfigError> {
            auto value = needsValue(i, arg);
            if (!value) { return Unexpected<ConfigError>{value.error()}; }
            if (auto applied = setValue(base, key, value.value()); !applied) {
                return Unexpected<ConfigError>{applied.error()};
            }
            ++i;
            return base;
        };

        if (arg == "--config") {
            auto value = needsValue(i, arg);
            if (!value) { return Unexpected<ConfigError>{value.error()}; }
            auto applied = applyFile(std::move(base), std::filesystem::path{value.value()});
            if (!applied) { return applied; }
            base = applied.value();
            ++i;
            continue;
        }
        if (arg == "--replay")    { auto r = applySimple("replay.path");    if (!r) { return r; } base = r.value(); continue; }
        if (arg == "--speed")     { auto r = applySimple("replay.speed");   if (!r) { return r; } base = r.value(); continue; }
        if (arg == "--port")      { auto r = applySimple("net.port");       if (!r) { return r; } base = r.value(); continue; }
        if (arg == "--bind")      { auto r = applySimple("net.bind");       if (!r) { return r; } base = r.value(); continue; }
        if (arg == "--log-level") { auto r = applySimple("logging.level");  if (!r) { return r; } base = r.value(); continue; }

        if (arg == "--set") {
            auto value = needsValue(i, arg);
            if (!value) { return Unexpected<ConfigError>{value.error()}; }
            const auto& pair     = value.value();
            const auto  separator = pair.find('=');
            if (separator == std::string::npos) {
                return Unexpected<ConfigError>{makeError(
                    ConfigErrorKind::InvalidValue, "--set", pair,
                    "un par clave=valor, p. ej. --set net.poolSize=8192")};
            }
            if (auto applied = setValue(base, pair.substr(0, separator), pair.substr(separator + 1));
                !applied) {
                return Unexpected<ConfigError>{applied.error()};
            }
            ++i;
            continue;
        }

        return Unexpected<ConfigError>{makeError(
            ConfigErrorKind::UnknownArgument, "", arg,
            "ejecute --help para ver los argumentos aceptados")};
    }
    return base;
}

// ── Validacion ───────────────────────────────────────────────────────────────

Result<void, ConfigError> validate(const Config& config)
{
    if (config.net.port == 0) {
        return invalid("net.port", "0", "un puerto entre 1 y 65535; el juego usa 20777 por defecto");
    }
    if (config.net.bind.empty()) {
        return invalid("net.bind", "", "127.0.0.1 para el juego en la misma maquina, "
                                       "o 0.0.0.0 para recibir de una consola de la LAN");
    }
    if (config.net.receiveBufferBytes < 65536) {
        return invalid("net.receiveBufferBytes", std::to_string(config.net.receiveBufferBytes),
                       "al menos 65536; el valor recomendado es 4194304 (RNF-026)");
    }
    // El minimo lo fija el MODELO DE PROPIEDAD, no la rafaga: ver la deduccion
    // completa junto a kMinimumPoolSize en config.hpp. Por debajo de ese numero,
    // un consumidor atascado agota el pool y se lleva por delante al otro.
    if (config.net.poolSize < kMinimumPoolSize) {
        return invalid("net.poolSize", std::to_string(config.net.poolSize),
                       "al menos " + std::to_string(kMinimumPoolSize) +
                           ", que es la suma de las capacidades de Qraw y Qrec mas "
                           "los buffers en vuelo; el valor por defecto es " +
                           std::to_string(kDefaultPoolSize));
    }
    if (config.net.silenceTimeoutMs < kMinSilenceTimeoutMs
        || config.net.silenceTimeoutMs > kMaxSilenceTimeoutMs) {
        return invalid("net.silenceTimeoutMs", std::to_string(config.net.silenceTimeoutMs),
                       "entre " + std::to_string(kMinSilenceTimeoutMs) + " y " +
                           std::to_string(kMaxSilenceTimeoutMs) +
                           " ms: por debajo, el ciclo de espera de ~100 ms no daria mas resolucion; "
                           "por encima, no quedaria margen para que la interfaz refleje la "
                           "inactividad en los 3 s de RF-012. El valor por defecto es " +
                           std::to_string(kDefaultSilenceTimeoutMs));
    }
    if (config.recording.compressionLevel < 1 || config.recording.compressionLevel > 19) {
        return invalid("recording.compressionLevel", std::to_string(config.recording.compressionLevel),
                       "un nivel zstd entre 1 y 19; el proyecto usa 3 (RF-402)");
    }
    if (!obs::parseLogLevel(config.logging.level).has_value()) {
        return invalid("logging.level", config.logging.level,
                       "trace, debug, info, warn, error u off");
    }
    for (const auto& [component, level] : config.logging.overrides) {
        if (!obs::parseLogLevel(level).has_value()) {
            return invalid("logging.overrides." + component, level,
                           "trace, debug, info, warn, error u off");
        }
    }
    if (config.shell.uiSource != "embedded" && config.shell.uiSource != "dev-server") {
        return invalid("shell.uiSource", config.shell.uiSource,
                       "embedded para la interfaz compilada, o dev-server para Vite");
    }
    if (config.transport.timingRateHz == 0 || config.transport.telemetryRateHz == 0) {
        return invalid("transport", "0", "un ritmo mayor que 0 Hz en timingRateHz y telemetryRateHz");
    }
    if (config.mode == Mode::Replay && config.replay.path.empty()) {
        return invalid("replay.path", "", "la ruta de una captura .f1raw cuando se usa --replay");
    }
    if (config.mode == Mode::Live && !config.replay.path.empty()) {
        return invalid("replay.path", config.replay.path,
                       "modo replay: indique --replay para activarlo");
    }
    if (config.replay.speed != kSpeedMax && (config.replay.speed <= 0.0 || config.replay.speed > 64.0)) {
        std::ostringstream found;
        found << config.replay.speed;
        return invalid("replay.speed", found.str(), "0.25x, 0.5x, 1x, 2x, 4x o max");
    }
    return {};
}

// ── Precedencia completa ─────────────────────────────────────────────────────

Result<Config, ConfigError> load(const std::vector<std::string>& args, const EnvLookup& env)
{
    auto config = defaults();

    // Precedencia: defectos -> fichero -> entorno -> argumentos.
    //
    // El fichero se aplica el PRIMERO aunque su ruta llegue en un argumento, de
    // modo que una variable de entorno pueda pisar lo que diga el fichero. Por
    // eso --config se resuelve aqui y se retira antes de procesar el resto.
    std::vector<std::string> remaining;
    remaining.reserve(args.size());

    std::optional<std::string> configPath;
    if (env) {
        if (auto fromEnv = env("PITWALL_CONFIG")) { configPath = *fromEnv; }
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--config") {
            if (i + 1 >= args.size()) {
                return Unexpected<ConfigError>{makeError(
                    ConfigErrorKind::InvalidValue, "--config", "<vacio>",
                    "la ruta de un fichero TOML a continuacion de --config")};
            }
            configPath = args[i + 1];
            ++i;
            continue;
        }
        remaining.push_back(args[i]);
    }

    if (configPath) {
        auto withFile = applyFile(std::move(config), std::filesystem::path{*configPath});
        if (!withFile) { return withFile; }
        config = withFile.value();
    }

    auto withEnvironment = applyEnvironment(std::move(config), env);
    if (!withEnvironment) { return withEnvironment; }

    auto withArguments = applyCommandLine(withEnvironment.value(), remaining);
    if (!withArguments) { return withArguments; }

    auto resolved = withArguments.value();
    if (auto valid = validate(resolved); !valid) {
        return Unexpected<ConfigError>{valid.error()};
    }
    return resolved;
}

} // namespace pw::config
