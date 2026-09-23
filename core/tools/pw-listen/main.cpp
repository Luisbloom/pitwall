// ─────────────────────────────────────────────────────────────────────────────
// pw-listen — escucha UDP e imprime estadisticas (F1.1-13).
//
// Es la primera prueba real contra el juego: demuestra que los bytes de F1 25
// entran bien por la capa de F1.1. NO es el producto -eso es PitWall.exe, en
// F1.6b- y NO decodifica nada: en esta fase solo sabemos recibir bytes.
//
// Este fichero se limita a los argumentos, a instalar el adaptador de la senal
// de parada y a devolver el codigo de salida. La logica vive en ListenApp para
// poder probarla sin lanzar procesos ni generar un Ctrl+C real.
// ─────────────────────────────────────────────────────────────────────────────
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "pw/config/config.hpp"
#include "listen_app.hpp"
#include "stop_signal.hpp"

int main(int argc, char** argv)
{
    const std::vector<std::string> args{argv + 1, argv + argc};
    const auto                     env = pw::config::systemEnv();

    auto loaded = pw::config::load(args, env);
    if (!loaded) {
        const auto& error = loaded.error();
        switch (error.kind) {
        case pw::config::ConfigErrorKind::HelpRequested:
            // La ayuda del nucleo nombra a pitwall-cli y anuncia opciones que
            // esta herramienta no soporta. El ANALISIS sigue siendo de
            // pw::config; solo el texto es propio.
            std::cout << pw::tools::listenUsageText();
            return pw::tools::kOk;
        case pw::config::ConfigErrorKind::VersionRequested:
            std::cout << error.describe() << '\n';
            return pw::tools::kOk;
        default:
            std::cerr << "pw-listen: " << error.describe() << '\n'
                      << "Ejecute --help para ver las opciones disponibles.\n";
            return pw::tools::kConfigError;
        }
    }

    try {
        // RAII: instala el manejador del sistema y lo restaura al salir.
        pw::tools::StopSignal stop;
        pw::tools::ListenApp  app{loaded.value(), stop, std::cout, std::cerr};
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << "pw-listen: fallo inesperado: " << error.what() << '\n';
        return pw::tools::kInternalError;
    }
}
