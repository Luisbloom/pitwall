#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::tools::LoadGenerator — emisor de carga UDP sostenida (F1.1-14).
//
// Existe para una sola cosa: comprobar RNF-001, «recepcion sostenida de al
// menos 1000 datagramas/s», que es 2,4 veces el pico real del juego a 60 Hz
// (~415/s, 01-REQUIREMENTS 12.6). Emite hacia pw-listen y despues se comparan
// los dos resumenes A MANO.
//
// NO es parte del producto y NO habla el protocolo de F1: el contenido es un
// patron determinista, no un paquete valido. Nada de decoder aqui.
//
// ── LO QUE ESTA HERRAMIENTA PUEDE Y NO PUEDE DEMOSTRAR ──────────────────────
//
// Cuenta lo que ENVIA. Lo que se recibe lo cuenta el receptor, y son dos cosas
// distintas: UDP no garantiza la entrega. Si en una ejecucion local coinciden,
// lo que se puede escribir es «0 perdidas OBSERVADAS en esta ejecucion», nunca
// «UDP no perdio nada».
//
// ── RITMO: LA FECHA LIMITE MANDA ────────────────────────────────────────────
//
// El n-esimo datagrama vence en `inicio + n * periodo`, con reloj monotono. No
// se duerme un periodo entre envios: eso acumularia el coste de cada vuelta y
// el error del planificador, y daria una tasa menor que nadie mediria.
//
// Si el sistema suspende el hilo, al despertar hay varios periodos vencidos:
// se recuperan hasta un TOPE por vuelta (kMaxCatchUpBurst) para no soltar una
// rafaga descontrolada. Si el retraso es tan grande que ni recuperando se
// alcanza (kResyncThresholdNs), se RENUNCIA a esos periodos, se resincroniza la
// fecha limite con el reloj y se cuentan como omitidos. Aparecen en el resumen:
// un retraso del sistema se ve, no se disimula.
//
// La tasa pedida y la conseguida son numeros distintos y se publican los dos.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "pw/core/result.hpp"
#include "pw/net/endpoint.hpp"
#include "pw/net/udp_socket.hpp"

namespace pw::tools {

class StopSignal;

/// Codigos de salida, los mismos que pw-listen.
enum LoadExit : int {
    kLoadOk            = 0,   ///< terminado o parado con Ctrl+C
    kLoadConfigError   = 1,   ///< argumentos invalidos
    kLoadInternalError = 2,   ///< error inesperado
    /// No se pudo enlazar, o ALGUN envio fallo. Un solo fallo invalida la
    /// ejecucion como prueba de carga, aunque el resto se enviara bien.
    kLoadSocketError   = 3,
};

/// Tamano por defecto del datagrama. Representativo de los paquetes grandes del
/// juego sin pretender ser ninguno de ellos.
inline constexpr std::size_t kDefaultPayloadBytes = 1200;

/// Minimo: la longitud minima que el receptor acepta (RF-004). Por debajo, todo
/// lo enviado contaria como truncado y la prueba no mediria lo que pretende.
inline constexpr std::size_t kMinPayloadBytes = 29;

/// Maximo: el datagrama mas grande del protocolo documentado, Participants 2026
/// (03-UDP-PROTOCOL 3). Se elige por COHERENCIA con lo que el receptor vera de
/// verdad, no por la MTU; ademas cabe sin fragmentar en Ethernet (1500 - 28 =
/// 1472) y sobra sitio en el buffer de 2048 B del pool.
inline constexpr std::size_t kMaxPayloadBytes = 1470;

struct LoadOptions {
    net::Ipv4Endpoint target{net::kLoopbackAddress, 20777};
    std::uint32_t     ratePerSecond{1000};
    std::uint32_t     durationSeconds{1800};   ///< 30 min: la prueba de RNF-001
    std::size_t       payloadBytes{kDefaultPayloadBytes};
};

struct LoadResult {
    std::uint64_t sent{0};             ///< datagramas que el socket acepto
    std::uint64_t sendFailures{0};     ///< envios que fallaron
    std::uint64_t skippedPeriods{0};   ///< periodos perdidos por retrasos del sistema
    std::uint64_t elapsedNs{0};
    double        achievedRate{0.0};   ///< enviados / duracion real
};

/// Ayuda de la herramienta.
[[nodiscard]] std::string loadUsageText();

/// Analiza los argumentos propios de pw-load.
///
/// Es un analizador minimo y DELIBERADAMENTE aparte de pw::config: rate,
/// duration y size no son configuracion del producto, son parametros de una
/// herramienta de prueba. El destino se da con --host y --port.
///
/// Devuelve el mensaje de error, ya accionable, cuando algo no es valido.
[[nodiscard]] Result<LoadOptions, std::string> parseLoadArgs(const std::vector<std::string>& args);

class LoadGenerator {
public:
    /// `socket` es la costura de pruebas: con nullptr se usa el socket real.
    LoadGenerator(LoadOptions options, StopSignal& stop, std::ostream& out, std::ostream& err,
                  std::unique_ptr<net::IUdpSocket> socket = nullptr);
    ~LoadGenerator();

    LoadGenerator(const LoadGenerator&)            = delete;
    LoadGenerator& operator=(const LoadGenerator&) = delete;

    /// Emite hasta agotar la duracion o hasta que se pida parar.
    [[nodiscard]] int run();

    [[nodiscard]] LoadResult result() const noexcept { return result_; }

private:
    LoadOptions                     options_;
    StopSignal&                     stop_;
    std::ostream&                   out_;
    std::ostream&                   err_;
    std::unique_ptr<net::IUdpSocket> socket_;
    LoadResult                      result_{};
};

} // namespace pw::tools
