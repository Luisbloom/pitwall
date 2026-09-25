#include "load_generator.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <thread>

#include "pw/core/time.hpp"
#include "stop_signal.hpp"

namespace pw::tools {

namespace {

/// Periodos vencidos que se recuperan como mucho en una vuelta. Sin tope, tras
/// una pausa larga del sistema se soltaria una rafaga enorme que no representa
/// nada de lo que se quiere medir.
constexpr std::uint64_t kMaxCatchUpBurst = 32;

/// Retraso a partir del cual se deja de recuperar y se resincroniza: 250 ms.
/// Los periodos que quedan dentro de ese hueco se cuentan como omitidos.
constexpr std::uint64_t kResyncThresholdNs = 250'000'000ULL;

/// Por encima de esta espera se duerme; por debajo se cede el turno y se vuelve
/// a mirar el reloj. Dormir con menos margen no es fiable: la granularidad del
/// temporizador del sistema es de varios milisegundos si nadie la ha bajado.
constexpr std::uint64_t kSleepThresholdNs = 2'000'000ULL;

[[nodiscard]] Result<std::uint64_t, std::string> parseNumber(const std::string& text,
                                                             const std::string& flag,
                                                             const std::uint64_t low,
                                                             const std::uint64_t high)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        return Unexpected<std::string>{flag + ": se esperaba un numero entero y se recibio '" + text
                                       + "'"};
    }
    std::uint64_t value = 0;
    for (const char digit : text) {
        value = value * 10 + static_cast<std::uint64_t>(digit - '0');
        if (value > high) { break; }
    }
    if (value < low || value > high) {
        return Unexpected<std::string>{flag + ": se esperaba un valor entre " + std::to_string(low)
                                       + " y " + std::to_string(high) + ", y se recibio '" + text
                                       + "'"};
    }
    return value;
}

std::string rateText(const double value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}

} // namespace

std::string loadUsageText()
{
    return "pw-load — emisor de carga UDP sostenida (F1.1-14)\n"
           "\n"
           "Uso: pw-load [opciones]\n"
           "\n"
           "  --host <ip>        Destino IPv4 (por defecto 127.0.0.1)\n"
           "  --port <n>         Puerto de destino (por defecto 20777)\n"
           "  --rate <n>         Datagramas por segundo (por defecto 1000)\n"
           "  --duration <s>     Segundos de emision (por defecto 1800, es decir 30 min)\n"
           "  --size <n>         Bytes por datagrama (por defecto 1200; entre 29 y 1470)\n"
           "  -h, --help         Mostrar esta ayuda\n"
           "  -v, --version      Mostrar la version\n"
           "\n"
           "El contenido es un patron determinista, NO un paquete valido de F1.\n"
           "Cuenta lo que envia; lo recibido lo cuenta pw-listen, y son cosas distintas:\n"
           "UDP no garantiza la entrega. Ctrl+C detiene la emision e imprime un resumen.\n";
}

Result<LoadOptions, std::string> parseLoadArgs(const std::vector<std::string>& args)
{
    LoadOptions options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];

        if (arg == "-h" || arg == "--help" || arg == "-v" || arg == "--version") {
            // Los gestiona main(): aqui solo se reconocen para no rechazarlos.
            continue;
        }

        if (i + 1 >= args.size()) {
            return Unexpected<std::string>{arg + ": falta su valor"};
        }
        const auto& value = args[++i];

        if (arg == "--host") {
            net::Ipv4Endpoint parsed;
            if (!net::Ipv4Endpoint::parse(value, options.target.port(), parsed)) {
                return Unexpected<std::string>{
                    "--host: se esperaba una IPv4 como 127.0.0.1 y se recibio '" + value + "'"};
            }
            options.target = net::Ipv4Endpoint{parsed.address(), options.target.port()};
            continue;
        }
        if (arg == "--port") {
            auto parsed = parseNumber(value, "--port", 1, 65535);
            if (!parsed) { return Unexpected<std::string>{parsed.error()}; }
            options.target = net::Ipv4Endpoint{options.target.address(),
                                               static_cast<std::uint16_t>(parsed.value())};
            continue;
        }
        if (arg == "--rate") {
            auto parsed = parseNumber(value, "--rate", 1, 1'000'000);
            if (!parsed) { return Unexpected<std::string>{parsed.error()}; }
            options.ratePerSecond = static_cast<std::uint32_t>(parsed.value());
            continue;
        }
        if (arg == "--duration") {
            auto parsed = parseNumber(value, "--duration", 1, 86'400);
            if (!parsed) { return Unexpected<std::string>{parsed.error()}; }
            options.durationSeconds = static_cast<std::uint32_t>(parsed.value());
            continue;
        }
        if (arg == "--size") {
            auto parsed = parseNumber(value, "--size", kMinPayloadBytes, kMaxPayloadBytes);
            if (!parsed) { return Unexpected<std::string>{parsed.error()}; }
            options.payloadBytes = static_cast<std::size_t>(parsed.value());
            continue;
        }

        return Unexpected<std::string>{"opcion desconocida: '" + arg
                                       + "'. Ejecute --help para ver las disponibles"};
    }

    return options;
}

LoadGenerator::LoadGenerator(LoadOptions options, StopSignal& stop, std::ostream& out,
                             std::ostream& err, std::unique_ptr<net::IUdpSocket> socket)
    : options_{options}, stop_{stop}, out_{out}, err_{err}, socket_{std::move(socket)}
{
}

LoadGenerator::~LoadGenerator() = default;

int LoadGenerator::run()
{
    auto socket = socket_ ? std::move(socket_) : net::makeUdpSocket();

    // Puerto efimero propio: el emisor no escucha, solo necesita un origen.
    if (const auto bound = socket->bind(net::Ipv4Endpoint{net::kAnyAddress, 0}); !bound) {
        err_ << "pw-load: " << bound.error().describe() << '\n';
        return kLoadSocketError;
    }

    // Payload preasignado UNA vez y reutilizado: 0 asignaciones por datagrama,
    // el mismo criterio que el receptor (RNF-013). El contenido es un patron
    // determinista con un contador de secuencia al principio.
    std::vector<std::byte> payload(options_.payloadBytes);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i & 0xFF);
    }

    const auto periodNs   = 1'000'000'000ULL / options_.ratePerSecond;
    const auto durationNs = static_cast<std::uint64_t>(options_.durationSeconds) * 1'000'000'000ULL;

    out_ << "pw-load — emitiendo hacia " << options_.target.toString() << '\n'
         << "  ritmo objetivo: " << options_.ratePerSecond << " datagramas/s ("
         << periodNs / 1000 << " us por datagrama)\n"
         << "  tamano: " << options_.payloadBytes << " B por datagrama (patron sintetico, NO F1)\n"
         << "  duracion: " << options_.durationSeconds << " s\n"
         << "  parada: " << StopSignal::backendName() << " — Ctrl+C para detener\n"
         << std::endl;

    const auto startNs = monotonicNowNs();
    const auto endNs   = startNs + durationNs;
    auto       nextNs  = startNs;
    std::uint64_t sequence = 0;

    while (!stop_.requested()) {
        const auto nowNs = monotonicNowNs();
        if (nowNs >= endNs) { break; }

        if (nowNs < nextNs) {
            // Todavia no toca. Dormir solo si hay margen de sobra; si no, ceder
            // el turno: por debajo de un par de milisegundos, dormir no es fiable.
            const auto remainingNs = nextNs - nowNs;
            if (remainingNs > kSleepThresholdNs) {
                std::this_thread::sleep_for(
                    std::chrono::nanoseconds{remainingNs - kSleepThresholdNs});
            } else {
                std::this_thread::yield();
            }
            continue;
        }

        // Periodos vencidos. Si el sistema nos suspendio mucho rato, se
        // renuncia a los que no caben y se resincroniza: el hueco se cuenta.
        auto lateNs = nowNs - nextNs;
        if (lateNs > kResyncThresholdNs) {
            const auto lost = lateNs / periodNs;
            result_.skippedPeriods += lost;
            nextNs += lost * periodNs;
            lateNs = nowNs - nextNs;
        }

        const auto due = std::min(kMaxCatchUpBurst, lateNs / periodNs + 1);
        for (std::uint64_t i = 0; i < due && !stop_.requested(); ++i) {
            // Contador de secuencia al principio del payload: deja comprobar
            // en las pruebas que cada datagrama es distinto y va en orden.
            std::memcpy(payload.data(), &sequence, sizeof(sequence));
            ++sequence;

            if (socket->sendTo(options_.target, payload)) {
                ++result_.sent;
            } else {
                ++result_.sendFailures;
            }
            nextNs += periodNs;
        }
    }

    result_.elapsedNs = monotonicNowNs() - startNs;
    result_.achievedRate =
        result_.elapsedNs == 0
            ? 0.0
            : static_cast<double>(result_.sent) * 1e9 / static_cast<double>(result_.elapsedNs);

    const auto stoppedBySignal = stop_.requested();

    out_ << "\n── resumen ──────────────────────────────────────────────────\n"
         << "  motivo: " << (stoppedBySignal ? "parada solicitada (Ctrl+C)" : "duracion agotada")
         << '\n'
         << "  destino: " << options_.target.toString() << '\n'
         << "  enviados: " << result_.sent << " datagramas de " << options_.payloadBytes << " B\n"
         << "  fallos de envio: " << result_.sendFailures << '\n'
         << "  duracion real: " << rateText(static_cast<double>(result_.elapsedNs) / 1e9) << " s\n"
         << "  ritmo objetivo: " << options_.ratePerSecond << " datagramas/s\n"
         << "  ritmo real: " << rateText(result_.achievedRate) << " datagramas/s\n"
         << "  periodos omitidos por retrasos del sistema: " << result_.skippedPeriods << '\n'
         << "\n  Lo anterior cuenta lo ENVIADO. Lo recibido lo cuenta pw-listen, y no es\n"
            "  lo mismo: UDP no garantiza la entrega.\n"
         << std::endl;

    // CUALQUIER fallo de envio invalida la ejecucion como prueba. pw-load existe
    // para validar RNF-001, y una carrera con un millon de envios correctos y
    // UNO fallido no es una ejecucion limpia: es una ejecucion con un fallo de
    // red que habria que explicar. Se siguen intentando los envios hasta el
    // final -las estadisticas parciales son utiles- pero el veredicto es 3.
    if (result_.sendFailures != 0) {
        err_ << "pw-load: " << result_.sendFailures << " de "
             << result_.sendFailures + result_.sent << " envios fallaron hacia "
             << options_.target.toString()
             << ". La ejecucion no es valida como prueba de carga\n";
        return kLoadSocketError;
    }
    return kLoadOk;
}

} // namespace pw::tools
