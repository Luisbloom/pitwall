#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::net::IUdpSocket — la capa de aislamiento de plataforma (F1.1-04, RNF-091).
//
// Winsock e implementacion POSIX detras de una interfaz comun. Es la UNICA
// frontera del nucleo donde entra una API del sistema operativo, y por eso es
// tambien el unico sitio del nucleo donde aparece una cabecera de Windows: en
// el fichero udp_socket_win32.cpp, que solo se compila en Windows.
//
// Windows/Winsock es la plataforma real del producto. La implementacion POSIX
// existe para que el nucleo siga compilando y ejecutandose en Linux (RNF-091),
// que es lo que permite la integracion continua con Clang y ThreadSanitizer.
//
// ── LO QUE ESTA CAPA NO HACE ────────────────────────────────────────────────
//
// No interpreta el contenido. Un datagrama son BYTES: ni cabecera, ni packetId,
// ni formato 2025 o 2026. Lo unico que mira de un datagrama es cuanto mide
// (04-ARCHITECTURE 3.1, 06-REALTIME-PIPELINE 2.2: «la unica validacion aqui es
// la longitud minima»).
//
// ── MODO DE OPERACION ───────────────────────────────────────────────────────
//
// Bloqueante con espera acotada (06-REALTIME-PIPELINE 2.1). El tiempo de espera
// no es una comodidad: es lo que permite que el bucle de recepcion compruebe su
// senal de parada sin depender de cerrar el socket desde otro hilo, que es
// carrera en las dos plataformas.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <type_traits>

#include "pw/core/result.hpp"
#include "pw/net/endpoint.hpp"

namespace pw::net {

enum class SocketErrorKind : std::uint8_t {
    AddressInUse,        ///< otro proceso ya escucha en ese puerto
    AddressNotAvailable, ///< la direccion no pertenece a esta maquina
    PermissionDenied,    ///< puertos < 1024, o cortafuegos
    InvalidAddress,      ///< la direccion de bind no es una IPv4 valida
    InvalidConfiguration,///< un parametro del receptor no permite cumplir su contrato
    NotInitialized,      ///< la pila de red no arranco (solo Winsock)
    Closed,              ///< el socket esta cerrado
    Timeout,             ///< la espera acotada expiro sin datos: NO es un fallo
    Oversized,           ///< el datagrama no cabia en el buffer y llego truncado
    Interrupted,
    Unknown
};

[[nodiscard]] const char* socketErrorName(SocketErrorKind kind) noexcept;

/// Texto por tipo, para los errores del camino caliente, que no llevan mensaje
/// propio: construirlo por datagrama asignaria memoria (RNF-013).
[[nodiscard]] const char* socketErrorAdvice(SocketErrorKind kind) noexcept;

/// Error de ARRANQUE: enlazar, configurar el socket. Lleva una frase accionable
/// porque alguien va a leerla y tiene que saber que hacer (RNF-097). Construirla
/// asigna memoria, y por eso este tipo NO se usa en el camino caliente.
struct SocketError {
    SocketErrorKind kind{SocketErrorKind::Unknown};
    int             nativeCode{0};  ///< WSAGetLastError() o errno, para el log
    std::string     message{};      ///< frase accionable: QUE pasa y QUE hacer

    [[nodiscard]] std::string describe() const;
};

/// Error del CAMINO CALIENTE: lo que puede devolver receive(), que se ejecuta
/// una vez por datagrama.
///
/// No lleva mensaje, y eso no es una omision: es lo que hace IMPOSIBLE que
/// asignar memoria dependa de la disciplina de quien lo construya. Un tipo
/// trivial no puede asignar.
///
/// La version anterior reutilizaba SocketError con el mensaje vacio. Bastaba
/// para no asignar en release, pero en Debug MSVC reserva 16 bytes de
/// «container proxy» al construir un std::string aunque este vacio: la
/// compilacion instrumentada lo detecto -3 a 8 asignaciones por medicion-, y la
/// solucion correcta no era tolerarlas sino quitar el std::string de en medio.
struct ReceiveError {
    SocketErrorKind kind{SocketErrorKind::Unknown};
    int             nativeCode{0};

    /// Texto por tipo. Asigna, asi que solo se llama al registrar o al mostrar.
    [[nodiscard]] std::string describe() const;
};

static_assert(std::is_trivially_copyable_v<ReceiveError>,
              "El error del camino caliente debe ser trivial: un tipo que no puede asignar "
              "memoria es mas fiable que un tipo que promete no hacerlo");

/// Resultado de recibir un datagrama.
struct ReceivedBytes {
    std::size_t  size{0};    ///< bytes escritos en el buffer
    Ipv4Endpoint origin{};   ///< de donde vino (RF-009)
};

class IUdpSocket {
public:
    IUdpSocket()                             = default;
    IUdpSocket(const IUdpSocket&)            = delete;
    IUdpSocket& operator=(const IUdpSocket&) = delete;
    virtual ~IUdpSocket()                    = default;

    /// Abre y enlaza. Un error aqui es de arranque y lleva mensaje accionable
    /// (RF-002, RF-003, RNF-097).
    [[nodiscard]] virtual Result<void, SocketError> bind(const Ipv4Endpoint& endpoint) = 0;

    /// Solicita `requested` bytes de buffer de recepcion y devuelve el valor que
    /// el sistema ha concedido de verdad, que puede ser otro (RNF-026, F1.1-06).
    [[nodiscard]] virtual Result<std::size_t, SocketError> setReceiveBufferBytes(
        std::size_t requested) = 0;

    /// Valor efectivo actual, leido del sistema con getsockopt.
    [[nodiscard]] virtual Result<std::size_t, SocketError> receiveBufferBytes() const = 0;

    /// Espera maxima ANTES de recibir: cuanto se espera a que el socket tenga
    /// algo que leer. 0 = esperar indefinidamente.
    ///
    /// El nombre dice «poll» a proposito. La version anterior se llamaba
    /// setReceiveTimeout, que sugiere SO_RCVTIMEO, y SO_RCVTIMEO es exactamente
    /// lo que NO se usa: con esa opcion se pierden datagramas al vencer la
    /// espera (06-REALTIME-PIPELINE.md 2.1.2, medido). La espera se hace con
    /// select() o poll() antes de recvfrom, que asi no vence nunca.
    [[nodiscard]] virtual Result<void, SocketError> setReceivePollTimeout(
        std::uint32_t milliseconds) = 0;

    /// Recibe UN datagrama en `buffer`. No asigna memoria.
    ///
    /// - Exito: bytes escritos y origen.
    /// - `Timeout`: expiro la espera sin datos. Es lo normal, no un fallo.
    /// - `Oversized`: el datagrama no cabia. Se ha perdido; se contabiliza.
    [[nodiscard]] virtual Result<ReceivedBytes, ReceiveError> receive(
        std::span<std::byte> buffer) = 0;

    /// Direccion efectiva tras el bind. Con puerto 0, aqui aparece el que
    /// concedio el sistema, que es como las pruebas evitan puertos fijos.
    [[nodiscard]] virtual Ipv4Endpoint localEndpoint() const noexcept = 0;

    /// Envia un datagrama. Existe para las pruebas y para el modo de inyeccion
    /// UDP del replay (RF-409); el receptor de produccion no envia nada.
    [[nodiscard]] virtual Result<std::size_t, SocketError> sendTo(
        const Ipv4Endpoint& destination, std::span<const std::byte> payload) = 0;

    virtual void               close() noexcept = 0;
    [[nodiscard]] virtual bool isOpen() const noexcept = 0;
};

/// Socket de la plataforma: Winsock en Windows, POSIX en el resto. Es el unico
/// punto del nucleo que decide en que sistema se esta ejecutando.
[[nodiscard]] std::unique_ptr<IUdpSocket> makeUdpSocket();

/// Nombre de la implementacion activa, para el log de arranque y las pruebas.
[[nodiscard]] const char* socketBackendName() noexcept;

} // namespace pw::net
