// Pruebas de la configuracion (F0-10): defectos, precedencia, validacion y
// mensajes de error accionables (RNF-097).
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "pw/config/config.hpp"

namespace {

using pw::config::Config;
using pw::config::Mode;

/// Entorno simulado: las pruebas no tocan el entorno real del proceso.
pw::config::EnvLookup fakeEnv(std::map<std::string, std::string> values)
{
    return [values = std::move(values)](
               const std::string_view name) -> std::optional<std::string> {
        const auto it = values.find(std::string{name});
        if (it == values.end()) { return std::nullopt; }
        return it->second;
    };
}

std::filesystem::path writeTempToml(const std::string& name, const std::string& content)
{
    const auto directory = std::filesystem::temp_directory_path() / "pitwall_test_config";
    std::filesystem::create_directories(directory);
    const auto path = directory / (name + ".toml");
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << content;
    return path;
}

bool contains(const std::string& haystack, const std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("Los valores por defecto son los documentados", "[config][defaults]")
{
    const auto config = pw::config::defaults();

    REQUIRE(config.mode == Mode::Live);
    REQUIRE(config.net.bind == "127.0.0.1");
    REQUIRE(config.net.port == 20777);
    REQUIRE(config.net.receiveBufferBytes == 4'194'304);
    REQUIRE(config.net.poolSize == 20480);
    REQUIRE(config.net.originFilter.empty());
    REQUIRE(config.net.silenceTimeoutMs == 2000);
    REQUIRE(config.recording.enabled);
    REQUIRE(config.recording.compressionLevel == 3);
    REQUIRE(config.recording.syncPointIntervalSeconds == 10);
    REQUIRE(config.transport.timingRateHz == 10);
    REQUIRE(config.transport.telemetryRateHz == 20);
    REQUIRE(config.shell.uiSource == "embedded");
    REQUIRE(config.logging.level == "info");
    REQUIRE(config.analysis.distanceStepMeters == 1.0);

    REQUIRE(pw::config::validate(config).has_value());
}

TEST_CASE("El fichero TOML sustituye a los valores por defecto", "[config][file]")
{
    const auto path = writeTempToml("basico", R"(
[net]
bind = "0.0.0.0"
port = 20778

[recording]
enabled = false

[logging]
level = "debug"
)");

    const auto loaded = pw::config::applyFile(pw::config::defaults(), path);
    REQUIRE(loaded.has_value());

    const auto& config = loaded.value();
    REQUIRE(config.net.bind == "0.0.0.0");
    REQUIRE(config.net.port == 20778);
    REQUIRE_FALSE(config.recording.enabled);
    REQUIRE(config.logging.level == "debug");
    REQUIRE(config.net.poolSize == 20480);         // lo no dicho conserva su defecto
    REQUIRE(config.sourceFile == path);
}

TEST_CASE("El fichero acepta listas y tablas anidadas", "[config][file]")
{
    const auto path = writeTempToml("listas", R"(
[net]
originFilter = ["127.0.0.1", "192.168.1.40"]

[logging.overrides]
"pw::state" = "trace"
"pw::decoder" = "debug"
)");

    const auto loaded = pw::config::applyFile(pw::config::defaults(), path);
    REQUIRE(loaded.has_value());

    const auto& config = loaded.value();
    REQUIRE(config.net.originFilter.size() == 2);
    REQUIRE(config.net.originFilter[0] == "127.0.0.1");
    REQUIRE(config.logging.overrides.at("pw::state") == "trace");
    REQUIRE(config.logging.overrides.at("pw::decoder") == "debug");
}

TEST_CASE("Los tres ficheros del repositorio son validos", "[config][file]")
{
    // config/default.toml documenta cada opcion: si deja de cargarse, la
    // referencia del proyecto ha dejado de ser cierta.
    const std::filesystem::path repositoryConfig{PW_TEST_CONFIG_DIR};

    for (const auto& name : {"default.toml", "dev.toml", "example.toml"}) {
        const auto path   = repositoryConfig / name;
        INFO("fichero: " << path.string());
        REQUIRE(std::filesystem::exists(path));

        const auto loaded = pw::config::applyFile(pw::config::defaults(), path);
        if (!loaded) { INFO(loaded.error().describe()); }
        REQUIRE(loaded.has_value());
        REQUIRE(pw::config::validate(loaded.value()).has_value());
    }
}

TEST_CASE("Las variables de entorno pisan al fichero", "[config][precedence]")
{
    const auto path = writeTempToml("precedencia_env", R"(
[net]
port = 20778
)");

    auto loaded = pw::config::applyFile(pw::config::defaults(), path);
    REQUIRE(loaded.has_value());

    const auto env = fakeEnv({{"PITWALL_NET_PORT", "20999"}});
    auto withEnv   = pw::config::applyEnvironment(loaded.value(), env);
    REQUIRE(withEnv.has_value());
    REQUIRE(withEnv.value().net.port == 20999);
}

TEST_CASE("Los argumentos pisan a las variables de entorno", "[config][precedence]")
{
    const auto env = fakeEnv({{"PITWALL_NET_PORT", "20999"},
                              {"PITWALL_LOGGING_LEVEL", "warn"}});

    const auto loaded = pw::config::load({"--port", "21000"}, env);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().net.port == 21000);
    REQUIRE(loaded.value().logging.level == "warn");   // el entorno sigue valiendo
}

TEST_CASE("La precedencia completa es defectos, fichero, entorno y argumentos",
          "[config][precedence]")
{
    const auto path = writeTempToml("precedencia_total", R"(
[net]
bind = "0.0.0.0"
port = 20778

[logging]
level = "error"
)");

    const auto env = fakeEnv({{"PITWALL_NET_PORT", "20999"},
                              {"PITWALL_LOGGING_LEVEL", "warn"}});

    const auto loaded = pw::config::load({"--config", path.string(), "--log-level", "debug"}, env);
    REQUIRE(loaded.has_value());

    const auto& config = loaded.value();
    REQUIRE(config.net.bind == "0.0.0.0");     // solo del fichero
    REQUIRE(config.net.port == 20999);         // entorno sobre fichero
    REQUIRE(config.logging.level == "debug");  // argumento sobre todo
    REQUIRE(config.net.poolSize == 20480);     // defecto intacto
}

TEST_CASE("Los argumentos documentados se aceptan", "[config][cli]")
{
    const auto env = fakeEnv({});

    const auto replay = pw::config::load(
        {"--replay", "tests/scenarios/2026_race_start.f1raw", "--speed", "0.5x", "--headless"}, env);
    REQUIRE(replay.has_value());
    REQUIRE(replay.value().mode == Mode::Replay);
    REQUIRE(replay.value().replay.speed == 0.5);
    REQUIRE(replay.value().headless);

    const auto maxSpeed = pw::config::load({"--replay", "captura.f1raw", "--speed", "max"}, env);
    REQUIRE(maxSpeed.has_value());
    REQUIRE(maxSpeed.value().replay.speed == pw::config::kSpeedMax);

    const auto noRecord = pw::config::load({"--no-record"}, env);
    REQUIRE(noRecord.has_value());
    REQUIRE_FALSE(noRecord.value().recording.enabled);

    const auto anyKey = pw::config::load({"--set", "net.poolSize=32768"}, env);
    REQUIRE(anyKey.has_value());
    REQUIRE(anyKey.value().net.poolSize == 32768);
}

// El nombre NO puede empezar por «--»: CTest lo pasa como argumento y Catch2
// lo tomaria por una opcion suya, imprimiria su ayuda y saldria con 0 sin
// ejecutar la prueba.
TEST_CASE("Las opciones --help y --version se distinguen de un error", "[config][cli]")
{
    const auto env = fakeEnv({});

    const auto help = pw::config::load({"--help"}, env);
    REQUIRE_FALSE(help.has_value());
    REQUIRE(help.error().kind == pw::config::ConfigErrorKind::HelpRequested);
    REQUIRE(contains(help.error().describe(), "--replay"));

    const auto version = pw::config::load({"--version"}, env);
    REQUIRE_FALSE(version.has_value());
    REQUIRE(version.error().kind == pw::config::ConfigErrorKind::VersionRequested);
}

TEST_CASE("Un argumento desconocido se rechaza y orienta", "[config][cli][RNF-097]")
{
    const auto loaded = pw::config::load({"--turbo"}, fakeEnv({}));

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().kind == pw::config::ConfigErrorKind::UnknownArgument);
    REQUIRE(contains(loaded.error().describe(), "--turbo"));
    REQUIRE(contains(loaded.error().describe(), "--help"));
}

TEST_CASE("Un fichero que no existe lo dice claramente", "[config][file][RNF-097]")
{
    const auto loaded = pw::config::load({"--config", "no/existe/pitwall.toml"}, fakeEnv({}));

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().kind == pw::config::ConfigErrorKind::FileNotFound);
    REQUIRE(contains(loaded.error().describe(), "no/existe/pitwall.toml"));
    REQUIRE(contains(loaded.error().describe(), "--config"));
}

TEST_CASE("Un TOML mal formado indica donde", "[config][file][RNF-097]")
{
    const auto path = writeTempToml("roto", "[net\nport = 20777\n");

    const auto loaded = pw::config::applyFile(pw::config::defaults(), path);
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().kind == pw::config::ConfigErrorKind::ParseError);
    REQUIRE(contains(loaded.error().describe(), "linea"));
}

TEST_CASE("Una clave desconocida no se ignora en silencio", "[config][file][RNF-097]")
{
    const auto path = writeTempToml("clave_rara", "[net]\nportt = 20777\n");

    const auto loaded = pw::config::applyFile(pw::config::defaults(), path);
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().kind == pw::config::ConfigErrorKind::UnknownKey);
    REQUIRE(contains(loaded.error().describe(), "net.portt"));
}

TEST_CASE("Un valor fuera de rango dice que se esperaba", "[config][validation][RNF-097]")
{
    const auto loaded = pw::config::load({"--port", "70000"}, fakeEnv({}));

    REQUIRE_FALSE(loaded.has_value());
    const auto message = loaded.error().describe();
    REQUIRE(loaded.error().kind == pw::config::ConfigErrorKind::InvalidValue);
    REQUIRE(loaded.error().key == "net.port");
    REQUIRE(loaded.error().found == "70000");
    REQUIRE(contains(message, "65535"));
}

TEST_CASE("Un valor de otro tipo dice que se esperaba", "[config][validation][RNF-097]")
{
    const auto loaded = pw::config::load({"--port", "veinte-mil"}, fakeEnv({}));

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(contains(loaded.error().describe(), "veinte-mil"));
    REQUIRE(contains(loaded.error().describe(), "entero"));
}

TEST_CASE("Un nivel de log invalido enumera los validos", "[config][validation][RNF-097]")
{
    const auto loaded = pw::config::load({"--log-level", "ruidoso"}, fakeEnv({}));

    REQUIRE_FALSE(loaded.has_value());
    const auto message = loaded.error().describe();
    REQUIRE(contains(message, "ruidoso"));
    REQUIRE(contains(message, "trace"));
    REQUIRE(contains(message, "error"));
}

TEST_CASE("Un error de entorno identifica la variable culpable",
          "[config][validation][RNF-097]")
{
    const auto env    = fakeEnv({{"PITWALL_NET_PORT", "0"}});
    const auto loaded = pw::config::load({}, env);

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(contains(loaded.error().describe(), "PITWALL_NET_PORT"));
}

TEST_CASE("Un pool por debajo del minimo se rechaza", "[config][validation][RNF-013]")
{
    // El antiguo valor por defecto, 8192, es exactamente el caso que hay que
    // rechazar: cabe una cola, no las dos.
    auto config         = pw::config::defaults();
    config.net.poolSize = 8192;

    const auto valid = pw::config::validate(config);
    REQUIRE_FALSE(valid.has_value());
    REQUIRE(valid.error().key == "net.poolSize");
    REQUIRE(contains(valid.error().describe(), "16387"));

    // Y el minimo por rafaga, que era el que se creia suficiente, tampoco basta.
    config.net.poolSize = 415 * 10;
    REQUIRE_FALSE(pw::config::validate(config).has_value());
}

TEST_CASE("El valor por defecto del pool cumple su propio minimo",
          "[config][validation][RNF-013]")
{
    // La prueba que impide que la discrepancia reaparezca. La comprobacion
    // fuerte, contra la deduccion de pw::mem, esta en
    // unit/config/test_pool_invariant.cpp.
    STATIC_REQUIRE(pw::config::kDefaultPoolSize >= pw::config::kMinimumPoolSize);

    const auto config = pw::config::defaults();
    REQUIRE(config.net.poolSize == pw::config::kDefaultPoolSize);
    REQUIRE(config.net.poolSize >= pw::config::kMinimumPoolSize);
    REQUIRE(pw::config::validate(config).has_value());
}

TEST_CASE("El silencio de RF-012 se configura dentro de su rango",
          "[config][validation][RF-012][F1.1-10]")
{
    REQUIRE(pw::config::defaults().net.silenceTimeoutMs == 2000);

    // Por argumentos: el valor se acepta y el resto no cambia.
    const auto overridden = pw::config::load({"--set", "net.silenceTimeoutMs=1500"}, fakeEnv({}));
    REQUIRE(overridden.has_value());
    REQUIRE(overridden.value().net.silenceTimeoutMs == 1500);

    // Y por entorno.
    const auto fromEnv =
        pw::config::load({}, fakeEnv({{"PITWALL_NET_SILENCETIMEOUTMS", "100"}}));
    REQUIRE(fromEnv.has_value());
    REQUIRE(fromEnv.value().net.silenceTimeoutMs == 100);

    // Los dos extremos del rango son validos; un paso fuera, no.
    for (const auto* accepted : {"100", "2000"}) {
        INFO("valor: " << accepted);
        auto config = pw::config::defaults();
        REQUIRE(pw::config::setValue(config, "net.silenceTimeoutMs", accepted).has_value());
        REQUIRE(pw::config::validate(config).has_value());
    }
    for (const auto* rejected : {"99", "2001", "0", "2400", "3000"}) {
        INFO("valor: " << rejected);
        const auto loaded = pw::config::load(
            {"--set", std::string{"net.silenceTimeoutMs="} + rejected}, fakeEnv({}));
        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().key == "net.silenceTimeoutMs");
        REQUIRE(loaded.error().kind == pw::config::ConfigErrorKind::InvalidValue);
    }

    // validate() tambien lo comprueba cuando el valor llega sin pasar por texto.
    auto config                 = pw::config::defaults();
    config.net.silenceTimeoutMs = 5000;
    const auto valid            = pw::config::validate(config);
    REQUIRE_FALSE(valid.has_value());
    REQUIRE(valid.error().key == "net.silenceTimeoutMs");
    REQUIRE(contains(valid.error().describe(), "RF-012"));
}

TEST_CASE("uiSource solo admite embedded o dev-server", "[config][validation][ADR-002]")
{
    const auto loaded = pw::config::load({"--set", "shell.uiSource=navegador"}, fakeEnv({}));

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(contains(loaded.error().describe(), "embedded"));
    REQUIRE(contains(loaded.error().describe(), "dev-server"));
}

TEST_CASE("El modo replay exige una captura", "[config][validation][ADR-007]")
{
    auto config  = pw::config::defaults();
    config.mode  = Mode::Replay;

    const auto valid = pw::config::validate(config);
    REQUIRE_FALSE(valid.has_value());
    REQUIRE(valid.error().key == "replay.path");
}

TEST_CASE("Toda clave documentada tiene variable de entorno", "[config][env]")
{
    const auto bindings = pw::config::keyBindings();
    REQUIRE(bindings.size() > 20);

    for (const auto& binding : bindings) {
        INFO("clave: " << binding.dottedKey);
        REQUIRE(binding.environmentVariable.rfind("PITWALL_", 0) == 0);
        REQUIRE_FALSE(binding.description.empty());

        // Toda clave anunciada debe existir de verdad.
        auto       config = pw::config::defaults();
        const auto result = pw::config::setValue(config, binding.dottedKey, "0");
        if (!result) {
            REQUIRE(result.error().kind != pw::config::ConfigErrorKind::UnknownKey);
        }
    }
}

TEST_CASE("expandPath resuelve las variables de las rutas", "[config][paths]")
{
    const auto env = fakeEnv({{"LOCALAPPDATA", "C:/Users/piloto/AppData/Local"}});

    const auto path = pw::config::expandPath("%LOCALAPPDATA%/PitWall/data/captures", env);
    REQUIRE(contains(path.generic_string(), "C:/Users/piloto/AppData/Local"));
    REQUIRE(contains(path.generic_string(), "PitWall/data/captures"));
}

TEST_CASE("expandPath tiene equivalente fuera de Windows", "[config][paths][RNF-091]")
{
    // El nucleo compila y se ejecuta tambien en Linux: una ruta con
    // %LOCALAPPDATA% no puede quedar vacia ahi.
    const auto env  = fakeEnv({{"HOME", "/home/piloto"}});
    const auto path = pw::config::expandPath("%LOCALAPPDATA%/PitWall/data", env);

    REQUIRE(contains(path.generic_string(), "/home/piloto"));
    REQUIRE(contains(path.generic_string(), "PitWall/data"));
}

TEST_CASE("Una ruta sin variables se deja intacta", "[config][paths]")
{
    const auto path = pw::config::expandPath("./data/captures", fakeEnv({}));
    REQUIRE(contains(path.generic_string(), "data/captures"));
}
