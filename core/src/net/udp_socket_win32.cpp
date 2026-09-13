// ─────────────────────────────────────────────────────────────────────────────
// Implementacion Winsock de IUdpSocket (F1.1-04). Windows es la plataforma real
// del producto.
//
// Este fichero es el UNICO del nucleo que incluye una cabecera de Windows, y
// solo se compila en Windows. La verificacion del grafo de dependencias
// (tools/check_deps.py) lo permite aqui por su sufijo _win32 y por estar en
// pw::net, que es la capa de aislamiento que exige RNF-091; el resto del nucleo
// sigue sin poder incluir nada de Windows.
// ─────────────────────────────────────────────────────────────────────────────
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "pw/net/udp_socket.hpp"

namespace pw::net {
namespace {

/// Arranque de la pila de Winsock. Es un recurso del PROCESO, no del socket:
/// se inicializa una vez, no se modifica despues y se libera al terminar. Es la
/// unica excepcion a «ninguna variable global mutable», y esta acotada a este
/// fichero.
class WinsockRuntime {
public:
    [[nodiscard]] static int ensure() noexcept
    {
        static WinsockRuntime instance;   // inicializacion unica y segura entre hilos
        return instance.result_;
    }

    WinsockRuntime(const WinsockRuntime&)            = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;

private:
    WinsockRuntime() noexcept
    {
        WSADATA data{};
        result_ = ::WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockRuntime()
    {
        if (result_ == 0) { ::WSACleanup(); }
    }

    int result_{0};
};

[[nodiscard]] SocketErrorKind classify(const int code) noexcept
{
    switch (code) {
    case WSAEADDRINUSE:    return SocketErrorKind::AddressInUse;
    case WSAEADDRNOTAVAIL: return SocketErrorKind::AddressNotAvailable;
    case WSAEACCES:        return SocketErrorKind::PermissionDenied;
    case WSAEFAULT:
    case WSAEAFNOSUPPORT:  return SocketErrorKind::InvalidAddress;
    case WSANOTINITIALISED:return SocketErrorKind::NotInitialized;
    case WSAENOTSOCK:
    case WSAESHUTDOWN:     return SocketErrorKind::Closed;
    case WSAETIMEDOUT:
    case WSAEWOULDBLOCK:   return SocketErrorKind::Timeout;
    case WSAEMSGSIZE:      return SocketErrorKind::Oversized;
    case WSAEINTR:         return SocketErrorKind::Interrupted;
    default:               return SocketErrorKind::Unknown;
    }
}

class WinsockUdpSocket final : public IUdpSocket {
public:
    ~WinsockUdpSocket() override { close(); }

    Result<void, SocketError> bind(const Ipv4Endpoint& endpoint) override
    {
        if (const auto startup = WinsockRuntime::ensure(); startup != 0) {
            return Unexpected<SocketError>{
                SocketError{SocketErrorKind::NotInitialized, startup,
                            "no se ha podido inicializar la pila de red de Windows. "
                            "Reinicie el equipo; si persiste, revise la instalacion de red"}};
        }

        close();

        handle_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (handle_ == INVALID_SOCKET) {
            const auto code = ::WSAGetLastError();
            return Unexpected<SocketError>{
                SocketError{classify(code), code, "no se ha podido crear el socket UDP"}};
        }

        // ── BIND EXCLUSIVO ──────────────────────────────────────────────
        //
        // SO_REUSEADDR NO se activa, ni aqui ni en POSIX. PitWall quiere lo
        // contrario: que el puerto sea suyo y que «ocupado» se detecte. UDP no
        // tiene TIME_WAIT, asi que tampoco hace falta para reiniciar rapido.
        //
        // Y ademas se pide exclusividad explicita: sin SO_EXCLUSIVEADDRUSE, otro
        // proceso puede enlazar NUESTRO puerto si el pone SO_REUSEADDR, y a
        // partir de ahi cada uno recibiria una parte de los datagramas. Perdida
        // silenciosa, imposible de diagnosticar desde dentro.
        //
        // NO es mejor esfuerzo: es un REQUISITO DE ARRANQUE. Si el sistema
        // rechaza la opcion que pide exclusividad, PitWall no puede seguir
        // comportandose como si la tuviera -ni prometer que detectara un puerto
        // ocupado-, asi que se para aqui con un mensaje y no llega al bind.
        //
        // Esto es camino de arranque, no camino caliente: puede fallar, puede
        // explicarse y puede costar una asignacion.
        int exclusive = 1;
        if (::setsockopt(handle_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                         reinterpret_cast<const char*>(&exclusive),
                         sizeof(exclusive)) == SOCKET_ERROR) {
            const auto code = ::WSAGetLastError();
            const auto kind = classify(code);
            close();
            return Unexpected<SocketError>{SocketError{
                kind, code,
                "el sistema no ha concedido el uso exclusivo del puerto " +
                    std::to_string(endpoint.port()) +
                    ". Sin el, otro programa podria enlazar el mismo puerto y quedarse con parte "
                    "de los datagramas sin que PitWall pueda detectarlo. Compruebe si hay una "
                    "politica de red o un software de seguridad que lo impida, o use otro puerto "
                    "en [net].port y en el juego"}};
        }

        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_port        = ::htons(endpoint.port());
        address.sin_addr.s_addr = ::htonl(endpoint.toNetworkOrder());

        if (::bind(handle_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
            SOCKET_ERROR) {
            const auto code = ::WSAGetLastError();
            const auto kind = classify(code);
            close();
            return Unexpected<SocketError>{SocketError{kind, code, bindAdvice(kind, endpoint)}};
        }

        bound_ = endpoint;
        // Con puerto 0 el sistema concede uno: se lee para poder informarlo.
        sockaddr_in actual{};
        int         length = sizeof(actual);
        if (::getsockname(handle_, reinterpret_cast<sockaddr*>(&actual), &length) == 0) {
            bound_ = Ipv4Endpoint::fromNetworkOrder(::ntohl(actual.sin_addr.s_addr),
                                                    ::ntohs(actual.sin_port));
        }
        return {};
    }

    Result<std::size_t, SocketError> setReceiveBufferBytes(const std::size_t requested) override
    {
        if (!isOpen()) { return closedError(); }

        auto value = static_cast<int>(std::min<std::size_t>(requested, 0x7FFF'FFFF));
        if (::setsockopt(handle_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&value),
                         sizeof(value)) == SOCKET_ERROR) {
            const auto code = ::WSAGetLastError();
            return Unexpected<SocketError>{
                SocketError{classify(code), code,
                            "el sistema ha rechazado el tamano de buffer de recepcion solicitado"}};
        }
        // Lo pedido no es lo concedido: se devuelve lo que el sistema dice de
        // verdad, que es lo que exige F1.1-06.
        return receiveBufferBytes();
    }

    Result<std::size_t, SocketError> receiveBufferBytes() const override
    {
        if (!isOpen()) { return closedError(); }

        int value  = 0;
        int length = sizeof(value);
        if (::getsockopt(handle_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&value),
                         &length) == SOCKET_ERROR) {
            const auto code = ::WSAGetLastError();
            return Unexpected<SocketError>{
                SocketError{classify(code), code,
                            "no se ha podido leer el tamano efectivo del buffer de recepcion"}};
        }
        return static_cast<std::size_t>(value < 0 ? 0 : value);
    }

    /// La espera acotada NO se implementa con SO_RCVTIMEO, y es una decision
    /// medida, no una preferencia:
    ///
    ///   espera de 10 ms con SO_RCVTIMEO   16-18 de 20 datagramas recibidos
    ///   espera de 2000 ms con SO_RCVTIMEO 20 de 20
    ///   espera de 10 ms con select()      20 de 20
    ///
    /// Microsoft lo documenta: cuando un recv con SO_RCVTIMEO vence, «el estado
    /// del socket es indeterminado y no deberia usarse». En la practica, un
    /// datagrama que llega mientras la espera esta venciendo se pierde. Con
    /// datagramas espaciados -el arranque de una sesion, una pausa en el juego-
    /// eso es perdida real y silenciosa.
    ///
    /// Con select(), la espera ocurre ANTES de recibir: recvfrom solo se llama
    /// cuando el socket ya tiene un datagrama, y entonces no vence nunca.
    Result<void, SocketError> setReceivePollTimeout(const std::uint32_t milliseconds) override
    {
        if (!isOpen()) { return closedError(); }
        pollTimeoutMs_ = milliseconds;
        return {};
    }

    Result<ReceivedBytes, ReceiveError> receive(const std::span<std::byte> buffer) override
    {
        if (!isOpen()) {
            return Unexpected<ReceiveError>{ReceiveError{SocketErrorKind::Closed, 0}};
        }

        // Espera acotada de LECTURA, no de recepcion. Ver setReceivePollTimeout.
        if (pollTimeoutMs_ != 0) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(handle_, &readable);

            timeval timeout{};
            timeout.tv_sec  = static_cast<long>(pollTimeoutMs_ / 1000);
            timeout.tv_usec = static_cast<long>((pollTimeoutMs_ % 1000) * 1000);

            const auto ready = ::select(0, &readable, nullptr, nullptr, &timeout);
            if (ready == 0) {
                return Unexpected<ReceiveError>{ReceiveError{SocketErrorKind::Timeout, 0}};
            }
            if (ready == SOCKET_ERROR) {
                const auto code = ::WSAGetLastError();
                return Unexpected<ReceiveError>{ReceiveError{classify(code), code}};
            }
        }

        sockaddr_in from{};
        int         fromLength = sizeof(from);

        const auto received =
            ::recvfrom(handle_, reinterpret_cast<char*>(buffer.data()),
                       static_cast<int>(std::min<std::size_t>(buffer.size(), 0x7FFF'FFFF)), 0,
                       reinterpret_cast<sockaddr*>(&from), &fromLength);

        if (received == SOCKET_ERROR) {
            const auto code = ::WSAGetLastError();
            const auto kind = classify(code);
            // Timeout y Oversized son estados NORMALES de este bucle, no averias:
            // el mensaje se mantiene corto porque no se muestra al usuario.
            // ReceiveError es trivial: no puede asignar memoria (RNF-013).
            return Unexpected<ReceiveError>{ReceiveError{kind, code}};
        }

        return ReceivedBytes{static_cast<std::size_t>(received),
                             Ipv4Endpoint::fromNetworkOrder(::ntohl(from.sin_addr.s_addr),
                                                            ::ntohs(from.sin_port))};
    }

    Result<std::size_t, SocketError> sendTo(const Ipv4Endpoint&             destination,
                                            const std::span<const std::byte> payload) override
    {
        if (!isOpen()) { return closedError(); }

        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_port        = ::htons(destination.port());
        address.sin_addr.s_addr = ::htonl(destination.toNetworkOrder());

        const auto sent =
            ::sendto(handle_, reinterpret_cast<const char*>(payload.data()),
                     static_cast<int>(std::min<std::size_t>(payload.size(), 0x7FFF'FFFF)), 0,
                     reinterpret_cast<const sockaddr*>(&address), sizeof(address));

        if (sent == SOCKET_ERROR) {
            const auto code = ::WSAGetLastError();
            return Unexpected<SocketError>{
                SocketError{classify(code), code, "fallo al enviar por el socket"}};
        }
        return static_cast<std::size_t>(sent);
    }

    Ipv4Endpoint localEndpoint() const noexcept override { return bound_; }

    void close() noexcept override
    {
        if (handle_ != INVALID_SOCKET) {
            ::closesocket(handle_);
            handle_ = INVALID_SOCKET;
        }
    }

    bool isOpen() const noexcept override { return handle_ != INVALID_SOCKET; }

private:
    [[nodiscard]] static Unexpected<SocketError> closedError() noexcept
    {
        return Unexpected<SocketError>{
            SocketError{SocketErrorKind::Closed, 0, "el socket no esta abierto"}};
    }

    /// El mensaje que exige RNF-097: no solo que ha fallado, sino que hacer.
    [[nodiscard]] static std::string bindAdvice(const SocketErrorKind kind,
                                                const Ipv4Endpoint&   endpoint)
    {
        const auto where = endpoint.toString();
        switch (kind) {
        case SocketErrorKind::AddressInUse:
            return "el puerto " + std::to_string(endpoint.port()) +
                   " ya lo esta usando otro programa. Las causas habituales son otra herramienta "
                   "de telemetria abierta, u otra instancia de PitWall ya en marcha. Cierrela, o "
                   "cambie [net].port en la configuracion y el puerto UDP del juego para que "
                   "coincidan";
        case SocketErrorKind::AddressNotAvailable:
            return "la direccion " + where +
                   " no pertenece a este equipo. Use 127.0.0.1 si el juego corre en este PC, "
                   "0.0.0.0 para recibir de una consola de la red local, o la IP concreta de una "
                   "de sus tarjetas de red";
        case SocketErrorKind::PermissionDenied:
            return "el sistema no permite escuchar en " + where +
                   ". Los puertos por debajo de 1024 requieren privilegios; use 20777, el valor "
                   "por defecto del juego. Compruebe tambien el cortafuegos de Windows";
        case SocketErrorKind::InvalidAddress:
            return "la direccion de escucha " + where + " no es una IPv4 valida";
        default:
            return "no se ha podido escuchar en " + where;
        }
    }

    SOCKET        handle_{INVALID_SOCKET};
    Ipv4Endpoint  bound_{};
    std::uint32_t pollTimeoutMs_{0};
};

} // namespace

std::unique_ptr<IUdpSocket> makeUdpSocket()
{
    return std::make_unique<WinsockUdpSocket>();
}

const char* socketBackendName() noexcept
{
    return "winsock";
}

} // namespace pw::net
