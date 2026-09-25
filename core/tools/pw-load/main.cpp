// ─────────────────────────────────────────────────────────────────────────────
// pw-load — emisor de carga UDP sostenida (F1.1-14).
//
// Sirve para comprobar RNF-001: recepcion sostenida de al menos 1000
// datagramas/s, 2,4 veces el pico real del juego. Se ejecuta contra pw-listen y
// despues se comparan los dos resumenes a mano.
//
// NO es parte del producto y NO habla el protocolo de F1.
// ─────────────────────────────────────────────────────────────────────────────
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "load_generator.hpp"
#include "stop_signal.hpp"

namespace {

constexpr std::string_view kVersion = "0.1.0";

bool hasFlag(const std::vector<std::string>& args, const std::string_view shortFlag,
             const std::string_view longFlag)
{
    for (const auto& arg : args) {
        if (arg == shortFlag || arg == longFlag) { return true; }
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> args{argv + 1, argv + argc};

    if (hasFlag(args, "-h", "--help")) {
        std::cout << pw::tools::loadUsageText();
        return pw::tools::kLoadOk;
    }
    if (hasFlag(args, "-v", "--version")) {
        std::cout << "pw-load " << kVersion << '\n';
        return pw::tools::kLoadOk;
    }

    auto options = pw::tools::parseLoadArgs(args);
    if (!options) {
        std::cerr << "pw-load: " << options.error() << '\n';
        return pw::tools::kLoadConfigError;
    }

    try {
        pw::tools::StopSignal    stop;
        pw::tools::LoadGenerator generator{options.value(), stop, std::cout, std::cerr};
        return generator.run();
    } catch (const std::exception& error) {
        std::cerr << "pw-load: fallo inesperado: " << error.what() << '\n';
        return pw::tools::kLoadInternalError;
    }
}
