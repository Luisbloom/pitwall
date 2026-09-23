// Pruebas de pw-load (F1.1-14).
//
// Todas duran milisegundos. La prueba que de verdad responde a RNF-001 -1000
// datagramas/s durante 30 minutos- es MANUAL y esta en 16-DEVELOPMENT-GUIDE §13.5:
// una suite automatica no puede demostrar una carga sostenida de media hora, y
// fingir que si seria peor que no tenerla.
//
// El ritmo se comprueba con tolerancia ancha a proposito: en una maquina cargada,
// exigir una cifra exacta produciria una prueba intermitente, que es la peor
// clase de prueba.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "pw/net/udp_socket.hpp"
#include "../net/scripted_socket.hpp"
#include "listen_app.hpp"
#include "load_generator.hpp"
#include "stop_signal.hpp"

using namespace std::chrono_literals;

using pw::tools::LoadGenerator;
using pw::tools::LoadOptions;
using pw::tools::StopSignal;

namespace {

bool contains(const std::string& haystack, const std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout = 5000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) { return true; }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

/// Socket que falla al enviar: siempre, o solo en el envio numero `failAt`.
///
/// El segundo caso es el que importa para el veredicto: una ejecucion con un
/// unico fallo entre muchos aciertos NO es una ejecucion limpia.
class FailingSender final : public pw::net::IUdpSocket {
public:
    FailingSender() = default;
    explicit FailingSender(const std::uint64_t failAt) noexcept : failAt_{failAt} {}

    pw::Result<void, pw::net::SocketError> bind(const pw::net::Ipv4Endpoint&) override
    {
        return {};
    }
    pw::Result<std::size_t, pw::net::SocketError> setReceiveBufferBytes(std::size_t n) override
    {
        return n;
    }
    pw::Result<std::size_t, pw::net::SocketError> receiveBufferBytes() const override
    {
        return std::size_t{0};
    }
    pw::Result<void, pw::net::SocketError> setReceivePollTimeout(std::uint32_t) override
    {
        return {};
    }
    pw::Result<pw::net::ReceivedBytes, pw::net::ReceiveError> receive(
        std::span<std::byte>) override
    {
        return pw::Unexpected<pw::net::ReceiveError>{
            pw::net::ReceiveError{pw::net::SocketErrorKind::Timeout, 0}};
    }
    pw::net::Ipv4Endpoint localEndpoint() const noexcept override { return {}; }
    pw::Result<std::size_t, pw::net::SocketError> sendTo(const pw::net::Ipv4Endpoint&,
                                                         const std::span<const std::byte> payload) override
    {
        ++attempts_;
        // failAt_ == 0: falla siempre. Si no, solo ese envio concreto.
        if (failAt_ == 0 || attempts_ == failAt_) {
            return pw::Unexpected<pw::net::SocketError>{
                pw::net::SocketError{pw::net::SocketErrorKind::Unknown, 0, "envio rechazado"}};
        }
        return payload.size();
    }
    void close() noexcept override {}
    bool isOpen() const noexcept override { return true; }

private:
    std::uint64_t failAt_{0};
    std::uint64_t attempts_{0};
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Argumentos
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Los valores por defecto de pw-load son los de RNF-001",
          "[tools][pw-load][F1.1-14][RNF-001]")
{
    const auto parsed = pw::tools::parseLoadArgs({});
    REQUIRE(parsed.has_value());

    const auto& options = parsed.value();
    REQUIRE(options.target.toString() == "127.0.0.1:20777");
    REQUIRE(options.ratePerSecond == 1000);      // 2,4x el pico real del juego
    REQUIRE(options.durationSeconds == 1800);    // 30 minutos
    REQUIRE(options.payloadBytes == pw::tools::kDefaultPayloadBytes);
    REQUIRE(pw::tools::kDefaultPayloadBytes == 1200);
}

TEST_CASE("pw-load acepta su linea de ordenes y rechaza lo invalido",
          "[tools][pw-load][F1.1-14]")
{
    const auto ok = pw::tools::parseLoadArgs(
        {"--host", "10.0.0.5", "--port", "20999", "--rate", "500", "--duration", "10", "--size",
         "300"});
    REQUIRE(ok.has_value());
    REQUIRE(ok.value().target.toString() == "10.0.0.5:20999");
    REQUIRE(ok.value().ratePerSecond == 500);
    REQUIRE(ok.value().durationSeconds == 10);
    REQUIRE(ok.value().payloadBytes == 300);

    struct Case {
        std::vector<std::string> args;
        std::string_view         mentions;
    };
    const std::vector<Case> rejected{
        {{"--host", "no-es-una-ip"}, "--host"},
        {{"--port", "0"}, "--port"},
        {{"--port", "70000"}, "--port"},
        {{"--rate", "0"}, "--rate"},
        {{"--duration", "0"}, "--duration"},
        {{"--size", "28"}, "--size"},      // por debajo de la longitud minima del receptor
        {{"--size", "1471"}, "--size"},    // por encima del mayor paquete documentado
        {{"--size"}, "falta su valor"},
        {{"--turbo", "1"}, "desconocida"},
    };
    for (const auto& testCase : rejected) {
        INFO("argumentos: " << testCase.args.front());
        const auto parsed = pw::tools::parseLoadArgs(testCase.args);
        REQUIRE_FALSE(parsed.has_value());
        REQUIRE(contains(parsed.error(), testCase.mentions));
    }

    // Los limites exactos si valen.
    for (const auto* size : {"29", "1470"}) {
        REQUIRE(pw::tools::parseLoadArgs({"--size", size}).has_value());
    }
}

TEST_CASE("La ayuda de pw-load es suya y avisa de lo que no es",
          "[tools][pw-load][F1.1-14]")
{
    const auto help = pw::tools::loadUsageText();
    REQUIRE(contains(help, "Uso: pw-load"));
    REQUIRE_FALSE(contains(help, "pitwall-cli"));
    for (const auto* option : {"--host", "--port", "--rate", "--duration", "--size"}) {
        REQUIRE(contains(help, option));
    }
    // Deja claro que no es protocolo y que enviar no es recibir.
    REQUIRE(contains(help, "NO un paquete valido de F1"));
    REQUIRE(contains(help, "UDP no garantiza la entrega"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Emision
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("El ritmo se planifica con fechas limite y se mide", "[tools][pw-load][F1.1-14]")
{
    // 200 datagramas/s durante 1 s: el objetivo son ~200. La banda es ancha a
    // proposito; lo que se comprueba es que planifica por tiempo y no gira sin
    // control, no una cifra exacta.
    auto receiver = pw::net::makeUdpSocket();
    REQUIRE(receiver->bind(pw::net::Ipv4Endpoint{pw::net::kLoopbackAddress, 0}).has_value());

    LoadOptions options;
    options.target          = receiver->localEndpoint();
    options.ratePerSecond   = 200;
    options.durationSeconds = 1;
    options.payloadBytes    = 64;

    std::ostringstream out;
    std::ostringstream err;
    StopSignal         stop;
    LoadGenerator      generator{options, stop, out, err};

    REQUIRE(generator.run() == pw::tools::kLoadOk);

    const auto result = generator.result();
    INFO("enviados " << result.sent << ", ritmo real " << result.achievedRate
                     << ", omitidos " << result.skippedPeriods);

    // Ni muy por debajo (giraria sin enviar) ni disparado (no respetaria el periodo).
    REQUIRE(result.sent >= 100);
    REQUIRE(result.sent <= 260);
    REQUIRE(result.achievedRate > 50.0);
    REQUIRE(result.achievedRate < 400.0);
    REQUIRE(result.sendFailures == 0);
    // Duracion real coherente con lo pedido: no termino antes de tiempo.
    REQUIRE(result.elapsedNs >= 900'000'000ULL);
    REQUIRE(err.str().empty());

    const auto text = out.str();
    REQUIRE(contains(text, "ritmo objetivo: 200"));
    REQUIRE(contains(text, "ritmo real"));
    REQUIRE(contains(text, "duracion agotada"));
    REQUIRE(contains(text, "UDP no garantiza la entrega"));
}

TEST_CASE("Cada datagrama lleva su numero de secuencia", "[tools][pw-load][F1.1-14]")
{
    auto receiver = pw::net::makeUdpSocket();
    REQUIRE(receiver->bind(pw::net::Ipv4Endpoint{pw::net::kLoopbackAddress, 0}).has_value());
    REQUIRE(receiver->setReceivePollTimeout(500).has_value());

    LoadOptions options;
    options.target          = receiver->localEndpoint();
    options.ratePerSecond   = 1000;
    options.durationSeconds = 1;
    options.payloadBytes    = 64;

    std::ostringstream out;
    std::ostringstream err;
    StopSignal         stop;
    LoadGenerator      generator{options, stop, out, err};

    std::thread sender{[&] { (void)generator.run(); }};

    std::array<std::byte, 2048> buffer{};
    std::uint64_t               previous = 0;
    for (int i = 0; i < 5; ++i) {
        const auto received = receiver->receive(buffer);
        REQUIRE(received.has_value());
        REQUIRE(received.value().size == 64);

        std::uint64_t sequence = 0;
        std::memcpy(&sequence, buffer.data(), sizeof(sequence));
        if (i > 0) { REQUIRE(sequence > previous); }   // estrictamente creciente
        previous = sequence;
    }

    stop.request();
    sender.join();
}

TEST_CASE("Una parada solicitada termina la emision antes de la duracion",
          "[tools][pw-load][F1.1-14]")
{
    auto receiver = pw::net::makeUdpSocket();
    REQUIRE(receiver->bind(pw::net::Ipv4Endpoint{pw::net::kLoopbackAddress, 0}).has_value());

    LoadOptions options;
    options.target          = receiver->localEndpoint();
    options.ratePerSecond   = 1000;
    options.durationSeconds = 600;   // diez minutos que NO se van a esperar
    options.payloadBytes    = 64;

    std::ostringstream out;
    std::ostringstream err;
    StopSignal         stop;
    LoadGenerator      generator{options, stop, out, err};

    int         exitCode = -1;
    std::thread sender{[&] { exitCode = generator.run(); }};

    REQUIRE(waitUntil([&] { return generator.result().sent > 0 || !out.str().empty(); }));
    std::this_thread::sleep_for(50ms);
    stop.request();
    sender.join();

    REQUIRE(exitCode == pw::tools::kLoadOk);
    REQUIRE(generator.result().elapsedNs < 600'000'000'000ULL);
    REQUIRE(contains(out.str(), "parada solicitada"));
}

TEST_CASE("Si el socket rechaza todos los envios, pw-load sale con 3",
          "[tools][pw-load][F1.1-14]")
{
    LoadOptions options;
    options.ratePerSecond   = 1000;
    options.durationSeconds = 1;
    options.payloadBytes    = 64;

    std::ostringstream out;
    std::ostringstream err;
    StopSignal         stop;
    LoadGenerator generator{options, stop, out, err, std::make_unique<FailingSender>()};

    REQUIRE(generator.run() == pw::tools::kLoadSocketError);
    REQUIRE(generator.result().sent == 0);
    REQUIRE(generator.result().sendFailures > 0);
    REQUIRE(contains(err.str(), "no es valida como prueba de carga"));
}

TEST_CASE("UN SOLO fallo de envio basta para invalidar la ejecucion",
          "[tools][pw-load][F1.1-14][RNF-001]")
{
    // El caso que de verdad fija la semantica: casi todo se envia bien y UNO
    // falla. Como prueba de RNF-001 eso no es una ejecucion limpia, asi que el
    // veredicto es 3 aunque las estadisticas sean buenas.
    LoadOptions options;
    options.ratePerSecond   = 500;
    options.durationSeconds = 1;
    options.payloadBytes    = 64;

    std::ostringstream out;
    std::ostringstream err;
    StopSignal         stop;
    LoadGenerator generator{options, stop, out, err, std::make_unique<FailingSender>(3)};

    REQUIRE(generator.run() == pw::tools::kLoadSocketError);

    const auto result = generator.result();
    INFO("enviados " << result.sent << ", fallos " << result.sendFailures);
    REQUIRE(result.sendFailures == 1);          // solo el tercero
    REQUIRE(result.sent > 10);                  // el resto se envio bien
    REQUIRE(contains(err.str(), "1 de "));      // el resumen de error cuenta los dos
    REQUIRE(contains(err.str(), "no es valida como prueba de carga"));

    // Y el resumen sigue dando todo lo util, no solo el error.
    const auto text = out.str();
    REQUIRE(contains(text, "fallos de envio: 1"));
    REQUIRE(contains(text, "ritmo objetivo: 500"));
    REQUIRE(contains(text, "ritmo real"));
    REQUIRE(contains(text, "periodos omitidos"));
}

TEST_CASE("Sin ningun fallo de envio, el codigo es 0", "[tools][pw-load][F1.1-14]")
{
    // El contrapunto: el mismo montaje, con un socket que nunca falla.
    LoadOptions options;
    options.ratePerSecond   = 500;
    options.durationSeconds = 1;
    options.payloadBytes    = 64;

    std::ostringstream out;
    std::ostringstream err;
    StopSignal         stop;
    // failAt fuera de alcance: no fallara ninguno.
    LoadGenerator generator{options, stop, out, err,
                            std::make_unique<FailingSender>(1'000'000'000ULL)};

    REQUIRE(generator.run() == pw::tools::kLoadOk);
    REQUIRE(generator.result().sendFailures == 0);
    REQUIRE(generator.result().sent > 10);
    REQUIRE(err.str().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Integracion corta: pw-load contra pw-listen, en bucle local
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pw-load y pw-listen juntos: sin descartes de la aplicacion",
          "[tools][pw-load][pw-listen][F1.1-14][RNF-001]")
{
    // Version corta -menos de un segundo- de la prueba manual de 30 minutos.
    // Demuestra el montaje y el contrato, NO RNF-001.
    auto config             = pw::config::defaults();
    config.net.bind         = "127.0.0.1";
    config.net.port         = 0;
    config.net.poolSize     = 4096;
    config.logging.console  = false;
    config.logging.directory.clear();

    std::ostringstream listenOut;
    std::ostringstream listenErr;
    StopSignal         stop;
    pw::tools::ListenApp listener{config, stop, listenOut, listenErr};
    listener.setPrintIntervalMs(100);

    int         listenExit = -1;
    std::thread listenThread{[&] { listenExit = listener.run(); }};
    REQUIRE(waitUntil([&] { return listener.listening(); }));

    LoadOptions options;
    options.target          = listener.boundEndpoint();
    options.ratePerSecond   = 1000;
    options.durationSeconds = 1;
    options.payloadBytes    = 1200;

    std::ostringstream loadOut;
    std::ostringstream loadErr;
    StopSignal         loadStop;   // comparte la bandera del proceso: se pide al final
    LoadGenerator      generator{options, loadStop, loadOut, loadErr};
    REQUIRE(generator.run() == pw::tools::kLoadOk);

    const auto sent = generator.result().sent;
    REQUIRE(sent > 100);

    // Un margen para que el receptor termine de drenar lo ultimo.
    std::this_thread::sleep_for(200ms);
    stop.request();
    listenThread.join();

    const auto text = listenOut.str();
    INFO("pw-load: " << loadOut.str() << "\npw-listen: " << text);

    REQUIRE(listenExit == pw::tools::kOk);
    // El criterio de F1.1-14: nada de lo que se pierda puede ser culpa nuestra.
    REQUIRE(contains(text, "Qraw 0, Qrec 0"));
    REQUIRE(contains(text, "pool agotado 0 veces"));
    REQUIRE(contains(text, "0 truncados, 0 oversized, 0 errores de socket"));
    REQUIRE(contains(text, "buffers en uso al terminar: 0"));
    REQUIRE(contains(text, "1200 B  x"));   // el tamano sintetico, observado
}
