#include "pw/net/udp_socket.hpp"

namespace pw::net {

const char* socketErrorName(const SocketErrorKind kind) noexcept
{
    switch (kind) {
    case SocketErrorKind::AddressInUse:        return "address_in_use";
    case SocketErrorKind::AddressNotAvailable: return "address_not_available";
    case SocketErrorKind::PermissionDenied:    return "permission_denied";
    case SocketErrorKind::InvalidAddress:      return "invalid_address";
    case SocketErrorKind::NotInitialized:      return "not_initialized";
    case SocketErrorKind::Closed:              return "closed";
    case SocketErrorKind::Timeout:             return "timeout";
    case SocketErrorKind::Oversized:           return "oversized";
    case SocketErrorKind::Interrupted:         return "interrupted";
    case SocketErrorKind::Unknown:             return "unknown";
    }
    return "unknown";
}

const char* socketErrorAdvice(const SocketErrorKind kind) noexcept
{
    switch (kind) {
    case SocketErrorKind::AddressInUse:        return "el puerto ya esta ocupado por otro programa";
    case SocketErrorKind::AddressNotAvailable: return "la direccion no pertenece a este equipo";
    case SocketErrorKind::PermissionDenied:    return "el sistema no permite escuchar ahi";
    case SocketErrorKind::InvalidAddress:      return "la direccion no es una IPv4 valida";
    case SocketErrorKind::NotInitialized:      return "la pila de red no esta inicializada";
    case SocketErrorKind::Closed:              return "el socket no esta abierto";
    case SocketErrorKind::Timeout:             return "sin datagramas en la espera";
    case SocketErrorKind::Oversized:           return "el datagrama no cabe en el buffer";
    case SocketErrorKind::Interrupted:         return "la operacion fue interrumpida";
    case SocketErrorKind::Unknown:             return "fallo del socket";
    }
    return "fallo del socket";
}

std::string SocketError::describe() const
{
    // Los errores del camino caliente no traen mensaje: construir un std::string
    // por datagrama asignaria memoria (RNF-013). El texto se pone aqui, que es
    // donde alguien va a leerlo.
    std::string out = message.empty() ? std::string{socketErrorAdvice(kind)} : message;
    if (nativeCode != 0) {
        out += " (codigo del sistema: ";
        out += std::to_string(nativeCode);
        out += ")";
    }
    return out;
}

std::string ReceiveError::describe() const
{
    std::string out{socketErrorAdvice(kind)};
    if (nativeCode != 0) {
        out += " (codigo del sistema: ";
        out += std::to_string(nativeCode);
        out += ")";
    }
    return out;
}

} // namespace pw::net
