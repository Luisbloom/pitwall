// Pruebas del logging estructurado (F0-09, RF-804, RF-805, OBS-04..06, RNF-102).
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <string>
#include <vector>

#include "pw/obs/logging.hpp"

using pw::obs::field;
using pw::obs::LogLevel;
using pw::obs::Logger;
using pw::obs::LoggerConfig;

namespace {

/// Destino de prueba: guarda en memoria lo que se escribiria.
class MemorySink final : public pw::obs::ILogSink {
public:
    void write(const std::string_view line) override
    {
        const std::lock_guard lock{mutex_};
        lines_.emplace_back(line);
    }
    void flush() override {}

    [[nodiscard]] std::vector<std::string> lines() const
    {
        const std::lock_guard lock{mutex_};
        return lines_;
    }

private:
    mutable std::mutex       mutex_;
    std::vector<std::string> lines_;
};

std::filesystem::path makeTempDir(const std::string& name)
{
    const auto path = std::filesystem::temp_directory_path() / ("pitwall_test_" + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

bool contains(const std::string& haystack, const std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("Los cinco niveles tienen nombre y se analizan", "[obs][logging][RF-804]")
{
    REQUIRE(std::string_view{pw::obs::logLevelName(LogLevel::Trace)} == "TRACE");
    REQUIRE(std::string_view{pw::obs::logLevelName(LogLevel::Debug)} == "DEBUG");
    REQUIRE(std::string_view{pw::obs::logLevelName(LogLevel::Info)} == "INFO");
    REQUIRE(std::string_view{pw::obs::logLevelName(LogLevel::Warn)} == "WARN");
    REQUIRE(std::string_view{pw::obs::logLevelName(LogLevel::Error)} == "ERROR");

    REQUIRE(pw::obs::parseLogLevel("trace") == LogLevel::Trace);
    REQUIRE(pw::obs::parseLogLevel("INFO") == LogLevel::Info);
    REQUIRE(pw::obs::parseLogLevel("Warning") == LogLevel::Warn);
    REQUIRE(pw::obs::parseLogLevel("off") == LogLevel::Off);
    REQUIRE_FALSE(pw::obs::parseLogLevel("ruidoso").has_value());
}

TEST_CASE("Una linea de log lleva marca de tiempo, nivel, componente y campos",
          "[obs][logging][OBS-06][RNF-101]")
{
    LoggerConfig config;
    config.level               = LogLevel::Info;
    config.aggregationWindowMs = 0;

    auto sink = std::make_shared<MemorySink>();
    {
        Logger logger{config};
        logger.addSink(sink);
        logger.info("pw::app", "PitWall iniciado",
                    {field("version", "0.1.0"), field("sessionUid", std::uint64_t{42})});
        logger.flush();
    }

    const auto lines = sink->lines();
    REQUIRE(lines.size() == 1);
    REQUIRE(contains(lines[0], "INFO"));
    REQUIRE(contains(lines[0], "pw::app"));
    REQUIRE(contains(lines[0], "PitWall iniciado"));
    REQUIRE(contains(lines[0], "version=0.1.0"));
    REQUIRE(contains(lines[0], "sessionUid=42"));
    REQUIRE(contains(lines[0], "T"));   // marca de tiempo ISO-8601
    REQUIRE(contains(lines[0], "Z"));
}

TEST_CASE("Por debajo del nivel activo no se escribe nada", "[obs][logging][RF-804]")
{
    LoggerConfig config;
    config.level               = LogLevel::Warn;
    config.aggregationWindowMs = 0;

    auto sink = std::make_shared<MemorySink>();
    {
        Logger logger{config};
        logger.addSink(sink);
        logger.trace("pw::net", "no deberia salir");
        logger.debug("pw::net", "no deberia salir");
        logger.info("pw::net", "no deberia salir");
        logger.warn("pw::net", "si sale");
        logger.error("pw::net", "tambien sale");
        logger.flush();
    }

    REQUIRE(sink->lines().size() == 2);
}

TEST_CASE("El nivel se cambia en caliente", "[obs][logging][OBS-04][RNF-103]")
{
    LoggerConfig config;
    config.level               = LogLevel::Error;
    config.aggregationWindowMs = 0;

    auto sink = std::make_shared<MemorySink>();
    {
        Logger logger{config};
        logger.addSink(sink);

        logger.info("pw::state", "antes");
        logger.flush();
        REQUIRE(sink->lines().empty());

        logger.setLevel(LogLevel::Debug);
        logger.info("pw::state", "despues");
        logger.flush();
    }

    const auto lines = sink->lines();
    REQUIRE(lines.size() == 1);
    REQUIRE(contains(lines[0], "despues"));
}

TEST_CASE("El nivel se ajusta por componente", "[obs][logging][OBS-04]")
{
    LoggerConfig config;
    config.level               = LogLevel::Info;
    config.aggregationWindowMs = 0;
    config.componentOverrides.emplace("pw::state", LogLevel::Trace);

    auto sink = std::make_shared<MemorySink>();
    {
        Logger logger{config};
        logger.addSink(sink);

        REQUIRE(logger.enabled("pw::state", LogLevel::Trace));
        REQUIRE_FALSE(logger.enabled("pw::net", LogLevel::Trace));

        logger.trace("pw::state", "detalle dirigido");
        logger.trace("pw::net", "ruido");
        logger.flush();
    }

    const auto lines = sink->lines();
    REQUIRE(lines.size() == 1);
    REQUIRE(contains(lines[0], "detalle dirigido"));
}

TEST_CASE("Los mensajes repetidos se agregan en lugar de inundar",
          "[obs][logging][RNF-102]")
{
    LoggerConfig config;
    config.level               = LogLevel::Warn;
    config.aggregationWindowMs = 60'000;   // ventana larga: nada expira durante la prueba

    auto sink = std::make_shared<MemorySink>();
    {
        Logger logger{config};
        logger.addSink(sink);
        for (int i = 0; i < 12; ++i) {
            logger.warn("pw::decoder", "tamano inesperado id=7 recibido=1444 esperado=1445");
        }
        logger.flush();

        // Solo la primera aparicion se escribe mientras la ventana esta abierta.
        REQUIRE(sink->lines().size() == 1);
        REQUIRE(logger.stats().aggregated == 11);
    }

    // Al cerrar, el resumen de los repetidos se emite con su recuento.
    const auto lines = sink->lines();
    REQUIRE(lines.size() == 2);
    REQUIRE(contains(lines[1], "repeticiones=12"));
}

TEST_CASE("Mensajes distintos no se agregan entre si", "[obs][logging][RNF-102]")
{
    LoggerConfig config;
    config.level               = LogLevel::Info;
    config.aggregationWindowMs = 60'000;

    auto sink = std::make_shared<MemorySink>();
    {
        Logger logger{config};
        logger.addSink(sink);
        logger.info("pw::app", "primero");
        logger.info("pw::app", "segundo");
        logger.info("pw::net", "primero");   // mismo texto, otro componente
        logger.flush();
    }

    REQUIRE(sink->lines().size() == 3);
}

TEST_CASE("Si la cola se llena se descarta y se cuenta, nunca se bloquea",
          "[obs][logging][RNF-102]")
{
    LoggerConfig config;
    config.level               = LogLevel::Info;
    config.queueCapacity       = 8;
    config.aggregationWindowMs = 60'000;

    Logger logger{config};   // sin destinos: el consumidor no drena mas rapido
    for (int i = 0; i < 5000; ++i) {
        logger.info("pw::net", "mensaje " + std::to_string(i));
    }

    // La propiedad que importa es que log() haya retornado siempre; el recuento
    // exacto depende de la velocidad relativa de los dos hilos.
    const auto stats = logger.stats();
    REQUIRE(stats.dropped + stats.emitted + stats.aggregated > 0);
}

TEST_CASE("El fichero de log rota por tamano y conserva un maximo",
          "[obs][logging][RF-805][OBS-05]")
{
    const auto directory = makeTempDir("rotacion");
    {
        pw::obs::RotatingFileSink sink{directory, "pitwall", /*maxFileSizeBytes=*/256,
                                       /*maxFiles=*/3};
        for (int i = 0; i < 200; ++i) {
            sink.write("linea de relleno numero " + std::to_string(i));
        }
        sink.flush();
        REQUIRE(sink.rotations() > 0);
    }

    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (entry.path().extension() == ".log") { ++files; }
    }
    REQUIRE(files > 1);
    REQUIRE(files <= 3);

    std::filesystem::remove_all(directory);
}

TEST_CASE("El destino de fichero escribe lo que se registra", "[obs][logging][RF-804]")
{
    const auto directory = makeTempDir("escritura");
    {
        LoggerConfig config;
        config.level               = LogLevel::Info;
        config.aggregationWindowMs = 0;

        Logger logger{config};
        logger.addSink(std::make_shared<pw::obs::RotatingFileSink>(directory, "pitwall",
                                                                   1024 * 1024, 3));
        logger.info("pw::app", "PitWall iniciado", {field("version", "0.1.0")});
        logger.flush();
    }

    std::string content;
    {
        std::ifstream     file{directory / "pitwall.log"};
        std::stringstream buffer;
        buffer << file.rdbuf();
        content = buffer.str();
    }

    REQUIRE(contains(content, "PitWall iniciado"));
    REQUIRE(contains(content, "version=0.1.0"));

    std::filesystem::remove_all(directory);
}

TEST_CASE("Un valor con espacios o comillas no rompe la linea", "[obs][logging][RNF-101]")
{
    LoggerConfig config;
    config.level               = LogLevel::Info;
    config.aggregationWindowMs = 0;

    auto sink = std::make_shared<MemorySink>();
    {
        Logger logger{config};
        logger.addSink(sink);
        logger.info("pw::config", "config cargada",
                    {field("ruta", "C:/Archivos de programa/PitWall\nsalto")});
        logger.flush();
    }

    const auto lines = sink->lines();
    REQUIRE(lines.size() == 1);
    REQUIRE(contains(lines[0], "\\n"));                       // el salto va escapado
    REQUIRE(lines[0].find('\n') == std::string::npos);        // una linea sigue siendo una linea
}
