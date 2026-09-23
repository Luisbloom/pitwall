#include "listen_app.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <memory>
#include <ostream>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "pw/core/time.hpp"
#include "pw/mem/buffer_pool.hpp"
#include "pw/net/received_datagram.hpp"
#include "pw/net/udp_receiver.hpp"
#include "pw/obs/logging.hpp"
#include "pw/obs/metrics.hpp"
#include "stop_signal.hpp"

namespace pw::tools {

namespace {

constexpr std::string_view kComponent = "pw::tools";

std::uint64_t packEndpoint(const net::Ipv4Endpoint& endpoint) noexcept
{
    return (static_cast<std::uint64_t>(endpoint.toNetworkOrder()) << 16)
           | static_cast<std::uint64_t>(endpoint.port());
}

net::Ipv4Endpoint unpackEndpoint(const std::uint64_t packed) noexcept
{
    return net::Ipv4Endpoint::fromNetworkOrder(
        static_cast<std::uint32_t>((packed >> 16) & 0xFFFF'FFFFULL),
        static_cast<std::uint16_t>(packed & 0xFFFFULL));
}

std::string_view stateName(const net::UdpReceiverState state) noexcept
{
    switch (state) {
    case net::UdpReceiverState::Listening: return "escuchando";
    case net::UdpReceiverState::Receiving: return "recibiendo";
    case net::UdpReceiverState::Silent:    return "inactivo";
    }
    return "desconocido";
}

std::string_view priorityName(const net::ThreadPriorityResult result) noexcept
{
    switch (result) {
    case net::ThreadPriorityResult::NotAttempted: return "sin intentar";
    case net::ThreadPriorityResult::Raised:       return "elevada";
    case net::ThreadPriorityResult::Failed:       return "rechazada por el sistema";
    case net::ThreadPriorityResult::Unsupported:  return "no disponible en esta plataforma";
    }
    return "desconocida";
}

/// hh:mm:ss a partir de nanosegundos monotonos.
std::string elapsedText(const std::uint64_t ns)
{
    const auto total   = ns / 1'000'000'000ULL;
    const auto hours   = total / 3600;
    const auto minutes = (total % 3600) / 60;
    const auto seconds = total % 60;

    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2) << minutes << ':'
        << std::setw(2) << seconds;
    return out.str();
}

std::string kibText(const double bytes)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << (bytes / 1024.0);
    return out.str();
}

/// Registrador segun la configuracion, igual que en pitwall-cli.
std::unique_ptr<obs::Logger> makeLogger(const config::Config& config, const config::EnvLookup& env)
{
    obs::LoggerConfig loggerConfig;
    loggerConfig.level = obs::parseLogLevel(config.logging.level).value_or(obs::LogLevel::Info);
    for (const auto& [component, level] : config.logging.overrides) {
        if (const auto parsed = obs::parseLogLevel(level)) {
            loggerConfig.componentOverrides.emplace(component, *parsed);
        }
    }

    auto logger = std::make_unique<obs::Logger>(loggerConfig);
    if (!config.logging.directory.empty()) {
        const auto directory = config::expandPath(config.logging.directory, env);
        logger->addSink(std::make_shared<obs::RotatingFileSink>(
            directory, "pw-listen",
            static_cast<std::uint64_t>(config.logging.maxFileSizeMb) * 1024ULL * 1024ULL,
            config.logging.maxFiles));
    }
    return logger;
}

} // namespace

std::string listenUsageText()
{
    return "pw-listen — escucha UDP e imprime estadisticas (F1.1-13)\n"
           "\n"
           "Uso: pw-listen [opciones]\n"
           "\n"
           "  --config <ruta>       Fichero TOML de configuracion\n"
           "  --port <n>            Puerto UDP de escucha (por defecto 20777)\n"
           "  --bind <direccion>    Direccion de escucha (por defecto 127.0.0.1;\n"
           "                        use 0.0.0.0 para recibir de otra maquina)\n"
           "  --set <clave=valor>   Fijar cualquier clave, p. ej. --set net.poolSize=32768\n"
           "  -h, --help            Mostrar esta ayuda\n"
           "  -v, --version         Mostrar la version\n"
           "\n"
           "No decodifica los datagramas: en esta fase solo demuestra que se reciben.\n"
           "Ctrl+C detiene la escucha e imprime un resumen.\n";
}

ListenApp::ListenApp(config::Config config, StopSignal& stop, std::ostream& out, std::ostream& err,
                     std::unique_ptr<net::IUdpSocket> socket)
    : config_{std::move(config)}
    , socket_{std::move(socket)}
    , stop_{stop}
    , out_{out}
    , err_{err}
{
}

ListenApp::~ListenApp() = default;

net::Ipv4Endpoint ListenApp::boundEndpoint() const noexcept
{
    return unpackEndpoint(bound_.load(std::memory_order_acquire));
}

int ListenApp::run()
{
    // ── 1. Lo que esta herramienta no hace ──────────────────────────────────
    if (config_.mode == config::Mode::Replay) {
        err_ << "pw-listen: --replay no corresponde a esta herramienta: pw-listen escucha el "
                "socket UDP y no reproduce capturas. Para una captura, use pitwall-cli "
                "--replay <ruta> cuando el replay este implementado (F1.4).\n";
        return kConfigError;
    }

    auto              logger  = makeLogger(config_, config::systemEnv());
    obs::MetricsRegistry metrics;
    mem::BufferPool   pool{config_.net.poolSize, mem::kDefaultBufferBytes};
    auto              rawQueue = std::make_unique<net::RawDatagramQueue>();
    auto              recQueue = std::make_unique<net::RecDatagramQueue>();

    net::UdpReceiverConfig receiverConfig;
    receiverConfig.bindAddress        = config_.net.bind;
    receiverConfig.port               = config_.net.port;
    receiverConfig.receiveBufferBytes = config_.net.receiveBufferBytes;
    receiverConfig.silenceTimeoutMs   = config_.net.silenceTimeoutMs;

    net::UdpReceiver receiver{receiverConfig,
                              pool,
                              *rawQueue,
                              *recQueue,
                              metrics,
                              *logger,
                              socket_ ? std::move(socket_) : net::makeUdpSocket()};

    // ── 2. Enlazar ──────────────────────────────────────────────────────────
    const auto opened = receiver.open();
    if (!opened) {
        // El mensaje accionable ya lo trae SocketError: no se duplica aqui.
        err_ << "pw-listen: " << opened.error().describe() << '\n';
        return kSocketError;
    }
    bound_.store(packEndpoint(opened.value()), std::memory_order_release);

    out_ << "pw-listen — escuchando en " << opened.value().toString() << '\n'
         << "  buffer de recepcion: " << config_.net.receiveBufferBytes << " B solicitados, "
         << receiver.effectiveReceiveBufferBytes() << " B concedidos\n"
         << "  pool: " << pool.capacity() << " buffers de " << pool.bufferBytes() << " B\n"
         << "  colas: Qraw y Qrec de " << net::RawDatagramQueue::capacity() << " huecos\n"
         << "  silencio: " << receiverConfig.silenceTimeoutMs << " ms sin datagramas\n"
         << "  parada: " << StopSignal::backendName() << " — Ctrl+C para detener\n"
         << std::endl;

    // ── 3. Los tres hilos ───────────────────────────────────────────────────
    std::stop_source  stopper;
    std::atomic<bool> receiverReturned{false};
    std::atomic<bool> producerDone{false};

    std::thread t1{[&] {
        receiver.run(stopper.get_token());
        receiverReturned.store(true, std::memory_order_release);
    }};

    // Consumidor UNICO de Qraw y de Qrec. Nunca imprime: su ritmo no puede
    // depender de la consola.
    std::thread drainer{[&] {
        net::ReceivedDatagram datagram;
        for (;;) {
            bool worked = false;
            while (rawQueue->tryPop(datagram)) {
                pool.release(datagram.handle);
                worked = true;
            }
            while (recQueue->tryPop(datagram)) {
                pool.release(datagram.handle);
                worked = true;
            }
            if (worked) { continue; }
            // Sin trabajo: si el productor ya termino y las dos colas estan
            // vacias, no puede llegar nada mas.
            if (producerDone.load(std::memory_order_acquire) && rawQueue->empty()
                && recQueue->empty()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds{200});
        }
    }};

    listening_.store(true, std::memory_order_release);

    // ── 4. Bucle de control: mirar la bandera e imprimir ────────────────────
    const auto startedNs = monotonicNowNs();
    auto       lastNs    = startedNs;
    auto       lastStats = receiver.stats();

    while (!stop_.requested() && !receiverReturned.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});

        const auto nowNs = monotonicNowNs();
        if (nowNs - lastNs < static_cast<std::uint64_t>(printIntervalMs_) * 1'000'000ULL) {
            continue;
        }

        const auto stats   = receiver.stats();
        const auto seconds = static_cast<double>(nowNs - lastNs) / 1e9;
        const auto packets = static_cast<double>(stats.received - lastStats.received) / seconds;
        const auto bytes   = static_cast<double>(stats.bytes - lastStats.bytes) / seconds;

        out_ << '[' << elapsedText(nowNs - startedNs) << "] " << stateName(receiver.state())
             << "  " << std::fixed << std::setprecision(0) << packets << " pkt/s  "
             << kibText(bytes) << " KiB/s"
             << "  total=" << stats.received << "  bytes=" << stats.bytes
             << "  origenes=" << receiver.sources().size() << "  truncados=" << stats.truncated
             << "  oversized=" << stats.oversized << "  errores=" << stats.socketErrors
             << "  qraw_drop=" << stats.qrawDropped << "  qrec_drop=" << stats.qrecDropped
             << "  pool_agotado=" << stats.poolExhaustions << "  pool_en_uso=" << pool.inUse()
             << std::endl;

        receiver.reportPendingEvents();
        receiver.reportStateTransitions();

        lastNs    = nowNs;
        lastStats = stats;
    }

    // ── 5. Apagado, en el orden de F1.1-12 ──────────────────────────────────
    const bool stoppedBySignal = stop_.requested();

    stopper.request_stop();       // 1. pedir la parada
    t1.join();                    // 2. unir T1: ya no se produce nada
    receiver.close();             // 3. cerrar el socket, y solo ahora
    producerDone.store(true, std::memory_order_release);
    drainer.join();               // 4. el drenaje termina de vaciar y se une

    const auto inUse   = pool.inUse();
    const auto stats   = receiver.stats();
    const auto totalNs = monotonicNowNs() - startedNs;
    listening_.store(false, std::memory_order_release);

    receiver.reportPendingEvents();
    receiver.reportStateTransitions();

    // ── 6. Resumen ──────────────────────────────────────────────────────────
    const auto averageRate =
        totalNs == 0 ? 0.0 : static_cast<double>(stats.received) * 1e9 / static_cast<double>(totalNs);

    const char* reason = stoppedBySignal ? "parada solicitada (Ctrl+C)"
                         : receiver.eventSeen(net::ReceiverEvent::SocketClosed)
                             ? "el socket se cerro"
                             : "el receptor termino por si mismo";

    out_ << "\n── resumen ──────────────────────────────────────────────────\n"
         << "  motivo: " << reason << '\n'
         << "  activo: " << elapsedText(totalNs) << '\n'
         << "  endpoint: " << opened.value().toString() << '\n'
         << "  SO_RCVBUF: " << config_.net.receiveBufferBytes << " B solicitados, "
         << receiver.effectiveReceiveBufferBytes() << " B concedidos\n"
         << "  datagramas: " << stats.received << " recibidos, " << stats.delivered
         << " entregados (" << std::fixed << std::setprecision(1) << averageRate
         << " pkt/s de media)\n"
         << "  bytes: " << stats.bytes << '\n'
         << "  invalidos: " << stats.truncated << " truncados, " << stats.oversized
         << " oversized, " << stats.socketErrors << " errores de socket\n"
         << "  descartes: Qraw " << stats.qrawDropped << ", Qrec " << stats.qrecDropped
         << "; pool agotado " << stats.poolExhaustions << " veces\n"
         << "  estado final: " << stateName(receiver.state()) << " ("
         << receiver.silenceTransitions() << " periodos de inactividad, "
         << receiver.receivingTransitions() << " entradas en recepcion)\n"
         << "  prioridad de T1: " << priorityName(receiver.threadPriority()) << '\n'
         << "  buffers en uso al terminar: " << inUse << '\n';

    const auto sources = receiver.sources().snapshot();
    out_ << "  origenes (" << sources.size() << "):\n";
    for (const auto& source : sources) {
        out_ << "    " << source.endpoint.toString() << "  " << source.datagrams
             << " datagramas, " << source.bytes << " bytes\n";
    }
    if (const auto unregistered = receiver.sources().datagrams(net::kUnregisteredSource);
        unregistered != 0) {
        out_ << "    (sin registrar, tabla llena)  " << unregistered << " datagramas\n";
    }

    auto sizes = receiver.sizeCounts().snapshot();
    std::sort(sizes.begin(), sizes.end(),
              [](const auto& a, const auto& b) { return a.datagrams > b.datagrams; });
    out_ << "  tamanos observados (" << sizes.size() << " distintos):\n";
    for (std::size_t i = 0; i < sizes.size() && i < 10; ++i) {
        out_ << "    " << sizes[i].bytes << " B  x" << sizes[i].datagrams << '\n';
    }
    out_ << std::endl;

    logger->info(kComponent, "pw-listen detenido",
                 {obs::field("motivo", std::string_view{reason}),
                  obs::field("datagramas", stats.received),
                  obs::field("buffersEnUso", static_cast<std::uint64_t>(inUse))});
    logger->flush();

    // ── 7. Codigo de salida ─────────────────────────────────────────────────
    if (inUse != 0) {
        err_ << "pw-listen: quedan " << inUse
             << " buffers en uso tras drenar; esto es un fallo interno\n";
        return kInternalError;
    }
    if (!stoppedBySignal) {
        // Nadie pidio parar: el socket se cerro o fallo por su cuenta.
        return kSocketError;
    }
    return kOk;
}

} // namespace pw::tools
