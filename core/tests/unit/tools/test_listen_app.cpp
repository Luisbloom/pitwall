// Pruebas de pw-listen (F1.1-13).
//
// Ejercitan la logica REAL -socket de loopback, tres hilos, drenaje y apagado-
// sin lanzar el proceso y sin generar un Ctrl+C real: la parada se pide por la
// misma costura que usa el adaptador del sistema, StopSignal::request().
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "pw/config/config.hpp"
#include "pw/net/udp_socket.hpp"
#include "../net/scripted_socket.hpp"
#include "listen_app.hpp"
#include "stop_signal.hpp"

using namespace std::chrono_literals;

using pw::tools::ListenApp;
using pw::tools::StopSignal;
using pw::test::ScriptedDatagram;
using pw::test::ScriptedSocket;

namespace {

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

bool contains(const std::string& haystack, const std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

/// Configuracion minima para una prueba: puerto efimero y pool pequeno. El pool
/// de produccion son 20.480 buffers; aqui no hace falta reservar 40 MiB.
pw::config::Config testConfig()
{
    auto config             = pw::config::defaults();
    config.net.bind         = "127.0.0.1";
    config.net.port         = 0;
    config.net.poolSize     = 64;
    config.logging.console  = false;
    config.logging.directory.clear();
    return config;
}

} // namespace

TEST_CASE("La ayuda es la de pw-listen, no la del nucleo", "[tools][pw-listen][F1.1-13]")
{
    const auto help = pw::tools::listenUsageText();
    INFO(help);

    REQUIRE(contains(help, "Uso: pw-listen"));
    REQUIRE_FALSE(contains(help, "pitwall-cli"));
    // No se anuncia lo que esta herramienta no soporta.
    REQUIRE_FALSE(contains(help, "--replay"));
    REQUIRE_FALSE(contains(help, "--speed"));

    for (const auto* option : {"--config", "--port", "--bind", "--set", "--help", "--version"}) {
        INFO("opcion: " << option);
        REQUIRE(contains(help, option));
    }
}

TEST_CASE("Las opciones --help y --version salen con 0 y no arrancan nada",
          "[tools][pw-listen][F1.1-13]")
{
    const auto env  = pw::config::EnvLookup{[](std::string_view) { return std::optional<std::string>{}; }};
    const auto help = pw::config::load({"--help"}, env);
    REQUIRE_FALSE(help.has_value());
    REQUIRE(help.error().kind == pw::config::ConfigErrorKind::HelpRequested);

    const auto version = pw::config::load({"--version"}, env);
    REQUIRE_FALSE(version.has_value());
    REQUIRE(version.error().kind == pw::config::ConfigErrorKind::VersionRequested);

    // main() traduce las dos a kOk.
    STATIC_REQUIRE(pw::tools::kOk == 0);
}

TEST_CASE("Una configuracion invalida se rechaza antes de tocar el socket",
          "[tools][pw-listen][F1.1-13]")
{
    const auto env     = pw::config::EnvLookup{[](std::string_view) { return std::optional<std::string>{}; }};
    const auto invalid = pw::config::load({"--port", "70000"}, env);

    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().kind == pw::config::ConfigErrorKind::InvalidValue);
    STATIC_REQUIRE(pw::tools::kConfigError == 1);
}

// El nombre NO puede empezar por «--»: CTest pasa el nombre del caso como
// argumento y Catch2 lo tomaria por una opcion suya.
TEST_CASE("La opcion --replay se rechaza con un error accionable y codigo 1",
          "[tools][pw-listen][F1.1-13]")
{
    auto config      = testConfig();
    config.mode      = pw::config::Mode::Replay;
    config.replay.path = "captura.f1raw";

    std::ostringstream out;
    std::ostringstream err;
    StopSignal         stop;
    ListenApp          app{config, stop, out, err};

    REQUIRE(app.run() == pw::tools::kConfigError);
    REQUIRE(contains(err.str(), "--replay"));
    REQUIRE(contains(err.str(), "pw-listen"));
    REQUIRE(out.str().empty());   // no llego a escuchar
}

TEST_CASE("Un puerto ocupado sale con 3 y explica que hacer", "[tools][pw-listen][F1.1-13]")
{
    // Alguien ya escucha en ese puerto: es el caso de la DoD de F1.1.
    auto occupant = pw::net::makeUdpSocket();
    REQUIRE(occupant->bind(pw::net::Ipv4Endpoint{pw::net::kLoopbackAddress, 0}).has_value());
    const auto taken = occupant->localEndpoint();

    auto config     = testConfig();
    config.net.port = taken.port();

    std::ostringstream out;
    std::ostringstream err;
    StopSignal         stop;
    ListenApp          app{config, stop, out, err};

    REQUIRE(app.run() == pw::tools::kSocketError);
    INFO("stderr: " << err.str());
    REQUIRE(contains(err.str(), "pw-listen"));
    REQUIRE(contains(err.str(), std::to_string(taken.port())));
    REQUIRE(contains(err.str(), "otro programa"));   // el consejo accionable del socket
}

TEST_CASE("Escucha, recibe, drena y para limpio con la costura de parada",
          "[tools][pw-listen][F1.1-13][RNF-055]")
{
    auto config = testConfig();

    std::ostringstream out;
    std::ostringstream err;
    StopSignal         stop;
    ListenApp          app{config, stop, out, err};
    app.setPrintIntervalMs(50);   // para no esperar un segundo por linea

    int         exitCode = -1;
    std::thread worker{[&] { exitCode = app.run(); }};

    REQUIRE(waitUntil([&] { return app.listening(); }));
    const auto target = app.boundEndpoint();
    REQUIRE(target.port() != 0);

    auto sender = pw::net::makeUdpSocket();
    REQUIRE(sender->bind(pw::net::Ipv4Endpoint{pw::net::kLoopbackAddress, 0}).has_value());

    constexpr int                kDatagrams = 25;
    const std::vector<std::byte> payload(64, std::byte{0x5A});
    for (int i = 0; i < kDatagrams; ++i) {
        REQUIRE(sender->sendTo(target, payload).has_value());
    }

    // Se espera a que aparezca al menos una linea periodica: demuestra que el
    // hilo de control imprime mientras el de drenaje vacia las colas.
    REQUIRE(waitUntil([&] { return contains(out.str(), "pkt/s"); }));

    stop.request();   // la misma bandera que marcaria Ctrl+C
    worker.join();

    const auto text = out.str();
    INFO("salida:\n" << text);

    REQUIRE(exitCode == pw::tools::kOk);
    REQUIRE(contains(text, "escuchando en 127.0.0.1:"));
    REQUIRE(contains(text, "parada solicitada (Ctrl+C)"));
    REQUIRE(contains(text, "buffers en uso al terminar: 0"));   // drenaje completo
    REQUIRE(contains(text, "origenes (1)"));                    // el emisor de la prueba
    REQUIRE(contains(text, "64 B  x"));                         // tamanos observados
    REQUIRE(contains(text, "Qraw 0, Qrec 0"));                  // sin descartes de la herramienta
    REQUIRE(contains(text, "pool agotado 0 veces"));
    REQUIRE(err.str().empty());
}

TEST_CASE("Parar sin haber recibido nada tambien termina limpio",
          "[tools][pw-listen][F1.1-13]")
{
    auto config = testConfig();

    std::ostringstream out;
    std::ostringstream err;
    StopSignal         stop;
    ListenApp          app{config, stop, out, err};
    app.setPrintIntervalMs(50);

    int         exitCode = -1;
    std::thread worker{[&] { exitCode = app.run(); }};

    REQUIRE(waitUntil([&] { return app.listening(); }));
    stop.request();
    worker.join();

    const auto text = out.str();
    REQUIRE(exitCode == pw::tools::kOk);
    REQUIRE(contains(text, "escuchando"));                      // estado inicial, sin trafico
    REQUIRE(contains(text, "0 recibidos"));
    REQUIRE(contains(text, "buffers en uso al terminar: 0"));
    REQUIRE(contains(text, "origenes (0)"));
}

TEST_CASE("Si el socket termina por su cuenta, pw-listen sale con 3",
          "[tools][pw-listen][F1.1-13]")
{
    // Fallo de socket DURANTE la ejecucion, no al abrir: el guion entrega un
    // datagrama y despues el socket se cierra bajo el receptor. Nadie pidio
    // parar, asi que la salida no es normal.
    const pw::net::Ipv4Endpoint       game{{192, 168, 1, 50}, 20777};
    const std::vector<ScriptedDatagram> script{
        ScriptedDatagram{game, 64},
        pw::test::scriptedError(pw::net::SocketErrorKind::Closed),
    };

    std::ostringstream out;
    std::ostringstream err;
    StopSignal         stop;
    ListenApp          app{testConfig(), stop, out, err,
                  std::make_unique<ScriptedSocket>(script)};
    app.setPrintIntervalMs(50);

    const auto exitCode = app.run();

    const auto text = out.str();
    INFO("salida:\n" << text);
    REQUIRE(exitCode == pw::tools::kSocketError);
    REQUIRE(contains(text, "el socket se cerro"));
    REQUIRE(contains(text, "buffers en uso al terminar: 0"));   // el drenaje ocurrio igual
    REQUIRE(contains(text, "1 recibidos"));
    REQUIRE_FALSE(stop.requested());
}

TEST_CASE("La costura de parada es la misma bandera que usa el sistema",
          "[tools][pw-listen][F1.1-13]")
{
    StopSignal stop;
    REQUIRE_FALSE(stop.requested());
    stop.request();
    REQUIRE(stop.requested());

    // El adaptador tiene nombre, y es el de esta plataforma.
    const std::string backend{StopSignal::backendName()};
#if defined(_WIN32)
    REQUIRE(backend == "SetConsoleCtrlHandler");
#else
    REQUIRE(contains(backend, "sigaction"));
#endif
}
