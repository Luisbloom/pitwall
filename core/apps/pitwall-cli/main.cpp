// ─────────────────────────────────────────────────────────────────────────────
// pitwall-cli — el nucleo de PitWall sin interfaz (F0-14).
//
// Arranca, lee la configuracion, registra «PitWall iniciado» y termina limpio.
// Todavia no recibe nada: la recepcion UDP llega en F1.1.
//
// Este ejecutable NO es el producto. El producto es PitWall.exe: una ventana
// propia con la interfaz dentro (ADR-002 v2), y se construye en la fase F1.6b.
// pitwall-cli existe para las pruebas de integracion, la reproduccion por lotes
// y la integracion continua en Linux, y es la prueba practica de que el shell
// no contamina el nucleo (09-BACKEND-PLAN §4).
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "pw/config/config.hpp"
#include "pw/core/time.hpp"
#include "pw/obs/logging.hpp"
#include "pw/obs/metrics.hpp"

namespace {

constexpr std::string_view kComponent = "pw::app";
constexpr std::string_view kVersion   = "0.1.0";

enum ExitCode : int {
    kOk              = 0,
    kConfigError     = 1,
    kUnexpectedError = 2
};

/// Construye el registrador segun la configuracion ya validada.
std::unique_ptr<pw::obs::Logger> makeLogger(const pw::config::Config& config,
                                            const pw::config::EnvLookup& env)
{
    pw::obs::LoggerConfig loggerConfig;
    loggerConfig.level = pw::obs::parseLogLevel(config.logging.level).value_or(pw::obs::LogLevel::Info);
    for (const auto& [component, level] : config.logging.overrides) {
        if (const auto parsed = pw::obs::parseLogLevel(level)) {
            loggerConfig.componentOverrides.emplace(component, *parsed);
        }
    }

    auto logger = std::make_unique<pw::obs::Logger>(loggerConfig);

    if (!config.logging.directory.empty()) {
        const auto directory = pw::config::expandPath(config.logging.directory, env);
        logger->addSink(std::make_shared<pw::obs::RotatingFileSink>(
            directory, "pitwall",
            static_cast<std::uint64_t>(config.logging.maxFileSizeMb) * 1024ULL * 1024ULL,
            config.logging.maxFiles));
    }
    if (config.logging.console) {
        logger->addSink(std::make_shared<pw::obs::ConsoleSink>());
    }
    return logger;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> args{argv + 1, argv + argc};
    const auto                     env = pw::config::systemEnv();

    auto loaded = pw::config::load(args, env);
    if (!loaded) {
        const auto& error = loaded.error();
        switch (error.kind) {
        case pw::config::ConfigErrorKind::HelpRequested:
            std::cout << error.describe();
            return kOk;
        case pw::config::ConfigErrorKind::VersionRequested:
            std::cout << error.describe() << '\n';
            return kOk;
        default:
            std::cerr << "PitWall: " << error.describe() << '\n'
                      << "Ejecute --help para ver las opciones disponibles.\n";
            return kConfigError;
        }
    }

    const auto& config = loaded.value();

    try {
        const auto startedNs = pw::monotonicNowNs();

        auto                    logger = makeLogger(config, env);
        pw::obs::MetricsRegistry metrics;
        auto&                   startups = metrics.counter("app_startups_total");
        auto&                   startupDuration = metrics.histogram("app_startup_duration_ns");

        startups.inc();

        logger->info(kComponent, "PitWall iniciado",
                     {pw::obs::field("version", kVersion),
                      pw::obs::field("modo", config.mode == pw::config::Mode::Replay ? "replay" : "live"),
                      pw::obs::field("config", config.sourceFile.empty()
                                                   ? std::string{"<valores por defecto>"}
                                                   : config.sourceFile.generic_string()),
                      pw::obs::field("bind", config.net.bind + ":" + std::to_string(config.net.port)),
                      pw::obs::field("nivel", config.logging.level)});

        // F0 termina aqui: no hay nada que recibir todavia. La recepcion UDP
        // llega en F1.1 y el shell de escritorio en F1.6b.
        logger->debug(kComponent, "Sin fuente de datagramas: F1.1 aun no implementada");

        startupDuration.observe(pw::monotonicNowNs() - startedNs);

        logger->info(kComponent, "PitWall detenido",
                     {pw::obs::field("metricas", static_cast<std::uint64_t>(metrics.size()))});
        logger->flush();
        return kOk;
    } catch (const std::exception& error) {
        // Los fallos de arranque si son excepcionales (09-BACKEND-PLAN §5).
        std::cerr << "PitWall: fallo al arrancar: " << error.what() << '\n';
        return kUnexpectedError;
    }
}
