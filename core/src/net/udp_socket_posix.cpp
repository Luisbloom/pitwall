// ─────────────────────────────────────────────────────────────────────────────
// Implementacion POSIX de IUdpSocket (F1.1-04).
//
// No es la plataforma del producto: existe para que el nucleo compile y se
// ejecute en Linux (RNF-091), que es lo que permite la integracion continua con
// Clang y ThreadSanitizer. Es deliberadamente la misma forma que la version de
// Winsock, para que las diferencias entre plataformas se vean de un vistazo.
//
// AVISO HONESTO: este fichero no se ha compilado ni ejecutado todavia. No hay
// toolchain de Linux en la maquina de desarrollo, y la integracion continua
// -que es la que lo verificaria- sigue pendiente de que exista un remoto.
// ─────────────────────────────────────────────────────────────────────────────
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

#include "pw/net/udp_socket.hpp"

namespace pw::net {
namespace {

[[nodiscard]] SocketErrorKind classify(const int code) noexcept
{
    switch (code) {
    case EADDRINUSE:    return SocketErrorKind::AddressInUse;
    case EADDRNOTAVAIL: return SocketErrorKind::AddressNotAvailable;
    case EACCES:
    case EPERM:         return SocketErrorKind::PermissionDenied;
    case EFAULT:
    case EAFNOSUPPORT:  return SocketErrorKind::InvalidAddress;
    case EBADF:
    case ENOTSOCK:      return SocketErrorKind::Closed;
    case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case ETIMEDOUT:     return SocketErrorKind::Timeout;
    case EMSGSIZE:      return SocketErrorKind::Oversized;
    case EINTR:         return SocketErrorKind::Interrupted;
    default:            return SocketErrorKind::Unknown;
    }
}

class PosixUdpSocket final : public IUdpSocket {
public:
    ~PosixUdpSocket() override { close(); }

    Result<void, SocketError> bind(const Ipv4Endpoint& endpoint) override
    {
        close();

        handle_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (handle_ < 0) {
            const auto code = errno;
            return Unexpected<SocketError>{
                SocketError{classify(code), code, "no se ha podido crear el socket UDP"}};
        }

        // SO_REUSEADDR NO se activa, igual que en Windows.
        //
        // PitWall no lo necesita: enlaza UN socket, en un puerto fijo, una vez al
        // arrancar, con instancia unica (RF-625). UDP no tiene TIME_WAIT, asi que
        // no hace falta para reiniciar rapido; y activarlo solo ampliaria los
        // casos en los que otro proceso puede compartir el puerto, que es
        // exactamente lo contrario de lo que se quiere: bind exclusivo y
        // deteccion fiable de «puerto ocupado».
        //
        // Lo que NO se afirma aqui: que el bind por defecto de POSIX garantice
        // exclusividad frente a cualquier otro proceso. No la garantiza en todos
        // los casos -SO_REUSEPORT del otro lado, por ejemplo-, y este fichero
        // todavia no se ha compilado ni ejecutado en ningun Linux.

        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_port        = ::htons(endpoint.port());
        address.sin_addr.s_addr = ::htonl(endpoint.toNetworkOrder());

        if (::bind(handle_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
            const auto code = errno;
            const auto kind = classify(code);
            close();
            return Unexpected<SocketError>{SocketError{kind, code, bindAdvice(kind, endpoint)}};
        }

        bound_ = endpoint;
        sockaddr_in actual{};
        socklen_t   length = sizeof(actual);
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
        if (::setsockopt(handle_, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)) < 0) {
            const auto code = errno;
            return Unexpected<SocketError>{
                SocketError{classify(code), code,
                            "el sistema ha rechazado el tamano de buffer de recepcion solicitado"}};
        }
        // Linux DUPLICA lo pedido al informarlo, y lo acota por
        // /proc/sys/net/core/rmem_max. Por eso se lee lo concedido en vez de
        // suponerlo (F1.1-06).
        return receiveBufferBytes();
    }

    Result<std::size_t, SocketError> receiveBufferBytes() const override
    {
        if (!isOpen()) { return closedError(); }

        int       value  = 0;
        socklen_t length = sizeof(value);
        if (::getsockopt(handle_, SOL_SOCKET, SO_RCVBUF, &value, &length) < 0) {
            const auto code = errno;
            return Unexpected<SocketError>{
                SocketError{classify(code), code,
                            "no se ha podido leer el tamano efectivo del buffer de recepcion"}};
        }
        return static_cast<std::size_t>(value < 0 ? 0 : value);
    }

    /// La espera acotada se hace con poll(), no con SO_RCVTIMEO. En Windows esa
    /// opcion pierde datagramas -medido: 16-18 de 20 con espera de 10 ms- y las
    /// dos plataformas deben comportarse igual; ver udp_socket_win32.cpp. Aqui
    /// se usa poll() y no select() porque select() no admite descriptores por
    /// encima de FD_SETSIZE, y un proceso longevo puede llegar ahi.
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
            pollfd descriptor{};
            descriptor.fd     = handle_;
            descriptor.events = POLLIN;

            const auto ready = ::poll(&descriptor, 1, static_cast<int>(pollTimeoutMs_));
            if (ready == 0) {
                return Unexpected<ReceiveError>{ReceiveError{SocketErrorKind::Timeout, 0}};
            }
            if (ready < 0) {
                const auto code = errno;
                return Unexpected<ReceiveError>{ReceiveError{classify(code), code}};
            }
        }

        sockaddr_in from{};
        socklen_t   fromLength = sizeof(from);

        const auto received = ::recvfrom(handle_, buffer.data(), buffer.size(), MSG_TRUNC,
                                         reinterpret_cast<sockaddr*>(&from), &fromLength);

        if (received < 0) {
            const auto code = errno;
            const auto kind = classify(code);
            // ReceiveError es trivial: no puede asignar memoria (RNF-013).
            return Unexpected<ReceiveError>{ReceiveError{kind, code}};
        }

        // Con MSG_TRUNC, Linux devuelve el tamano REAL del datagrama aunque no
        // cupiera. Un datagrama mayor que el buffer se reporta como Oversized,
        // igual que hace Winsock con WSAEMSGSIZE, para que las dos plataformas
        // se comporten igual.
        if (static_cast<std::size_t>(received) > buffer.size()) {
            return Unexpected<ReceiveError>{ReceiveError{SocketErrorKind::Oversized, EMSGSIZE}};
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

        const auto sent = ::sendto(handle_, payload.data(), payload.size(), 0,
                                   reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (sent < 0) {
            const auto code = errno;
            return Unexpected<SocketError>{
                SocketError{classify(code), code, "fallo al enviar por el socket"}};
        }
        return static_cast<std::size_t>(sent);
    }

    Ipv4Endpoint localEndpoint() const noexcept override { return bound_; }

    void close() noexcept override
    {
        if (handle_ >= 0) {
            ::close(handle_);
            handle_ = -1;
        }
    }

    bool isOpen() const noexcept override { return handle_ >= 0; }

private:
    [[nodiscard]] static Unexpected<SocketError> closedError() noexcept
    {
        return Unexpected<SocketError>{
            SocketError{SocketErrorKind::Closed, 0, "el socket no esta abierto"}};
    }

    [[nodiscard]] static std::string bindAdvice(const SocketErrorKind kind,
                                                const Ipv4Endpoint&   endpoint)
    {
        const auto where = endpoint.toString();
        switch (kind) {
        case SocketErrorKind::AddressInUse:
            return "el puerto " + std::to_string(endpoint.port()) +
                   " ya lo esta usando otro programa. Cierrelo, o cambie [net].port en la "
                   "configuracion y el puerto UDP del juego para que coincidan";
        case SocketErrorKind::AddressNotAvailable:
            return "la direccion " + where +
                   " no pertenece a este equipo. Use 127.0.0.1, 0.0.0.0 o la IP de una de sus "
                   "interfaces";
        case SocketErrorKind::PermissionDenied:
            return "el sistema no permite escuchar en " + where +
                   ". Los puertos por debajo de 1024 requieren privilegios; use 20777";
        case SocketErrorKind::InvalidAddress:
            return "la direccion de escucha " + where + " no es una IPv4 valida";
        default:
            return "no se ha podido escuchar en " + where;
        }
    }

    int           handle_{-1};
    Ipv4Endpoint  bound_{};
    std::uint32_t pollTimeoutMs_{0};
};

} // namespace

std::unique_ptr<IUdpSocket> makeUdpSocket()
{
    return std::make_unique<PosixUdpSocket>();
}

const char* socketBackendName() noexcept
{
    return "posix";
}

} // namespace pw::net
