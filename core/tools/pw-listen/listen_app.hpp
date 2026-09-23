#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::tools::ListenApp — la logica de pw-listen (F1.1-13).
//
// Escucha UDP de verdad con la capa de F1.1, drena las dos colas, imprime
// estadisticas una vez por segundo y termina limpio cuando se pide parar. No
// decodifica NADA: en F1.1 solo sabemos recibir bytes de forma fiable, asi que
// no hay packetId, ni formato, ni sessionUID, ni telemetria. Eso es F1.2.
//
// Vive aparte de main.cpp para poder probarse sin lanzar procesos ni generar un
// Ctrl+C real: las pruebas construyen ListenApp, la ejecutan en un hilo y piden
// la parada por la misma costura que usa el adaptador del sistema.
//
// ── TRES HILOS ──────────────────────────────────────────────────────────────
//
//   control   el que llama a run(): mira la bandera de parada, calcula los
//             ritmos entre instantaneas e imprime. Coordina el apagado
//   T1        UdpReceiver::run(), y nada mas
//   drenaje   consumidor UNICO de Qraw y de Qrec (dos colas SPSC distintas, un
//             solo consumidor cada una): saca y suelta cada buffer
//
// El drenaje va en su propio hilo A PROPOSITO: si la consola se bloquea -en
// Windows basta con seleccionar texto- el drenaje no puede pararse, o pw-listen
// fabricaria descartes y agotamientos del pool que no son del receptor.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>

#include "pw/config/config.hpp"
#include "pw/net/endpoint.hpp"
#include "pw/net/udp_socket.hpp"

namespace pw::tools {

class StopSignal;

/// Ayuda de ESTA herramienta.
///
/// El analisis de argumentos sigue siendo exclusivamente pw::config -no hay un
/// segundo sistema de configuracion-, pero su texto de ayuda es el del nucleo
/// completo: nombra a pitwall-cli y anuncia opciones que pw-listen no soporta,
/// como --replay. Aqui solo se sustituye el TEXTO.
[[nodiscard]] std::string listenUsageText();

/// Codigos de salida del proceso.
enum ListenExit : int {
    kOk           = 0,   ///< parada normal (Ctrl+C), --help o --version
    kConfigError  = 1,   ///< configuracion invalida
    kInternalError = 2,  ///< error inesperado
    kSocketError  = 3,   ///< fallo de socket o de red: al abrir o en ejecucion
};

class ListenApp {
public:
    /// `socket` es una costura de pruebas: con nullptr se usa el socket real de
    /// la plataforma, que es lo que hace siempre el ejecutable.
    ListenApp(config::Config config, StopSignal& stop, std::ostream& out, std::ostream& err,
              std::unique_ptr<net::IUdpSocket> socket = nullptr);
    ~ListenApp();

    ListenApp(const ListenApp&)            = delete;
    ListenApp& operator=(const ListenApp&) = delete;

    /// Ejecuta la herramienta hasta que se pide parar o el socket termina.
    /// Devuelve el codigo de salida.
    [[nodiscard]] int run();

    /// Endpoint realmente enlazado; puerto 0 mientras no se haya enlazado.
    /// Seguro desde otro hilo: las pruebas lo usan para saber a donde enviar.
    [[nodiscard]] net::Ipv4Endpoint boundEndpoint() const noexcept;

    /// Cierto cuando el socket esta enlazado y los hilos corriendo.
    [[nodiscard]] bool listening() const noexcept
    {
        return listening_.load(std::memory_order_acquire);
    }

    /// Intervalo de impresion. Fijo en produccion; las pruebas lo acortan para
    /// no esperar un segundo por linea.
    void setPrintIntervalMs(std::uint32_t ms) noexcept { printIntervalMs_ = ms; }

private:
    struct Impl;

    config::Config                  config_;
    std::unique_ptr<net::IUdpSocket> socket_;
    StopSignal&    stop_;
    std::ostream&  out_;
    std::ostream&  err_;

    std::atomic<std::uint64_t> bound_{0};       ///< endpoint empaquetado
    std::atomic<bool>          listening_{false};
    std::uint32_t              printIntervalMs_{1000};
};

} // namespace pw::tools
