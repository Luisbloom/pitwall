#include "pw/core/reason.hpp"

namespace pw {

const char* reasonName(const Reason reason) noexcept
{
    switch (reason) {
    case Reason::None:                 return "none";
    case Reason::NotYetReceived:       return "not_yet_received";
    case Reason::NotSupportedByFormat: return "not_supported_by_format";
    case Reason::NotApplicable:        return "not_applicable";
    case Reason::RestrictedByOwner:    return "restricted_by_owner";
    case Reason::SentinelValue:        return "sentinel_value";
    case Reason::DecodeError:          return "decode_error";
    case Reason::Unknown:              return "unknown";
    }
    return "unknown";
}

const char* reasonDescription(const Reason reason) noexcept
{
    switch (reason) {
    case Reason::None:                 return "Dato presente";
    case Reason::NotYetReceived:       return "Todavia no se ha recibido";
    case Reason::NotSupportedByFormat: return "El formato en uso no proporciona este dato";
    case Reason::NotApplicable:        return "No aplicable en este contexto";
    case Reason::RestrictedByOwner:    return "Restringido por su propietario";
    case Reason::SentinelValue:        return "El juego lo emite como ausente";
    case Reason::DecodeError:          return "No se pudo decodificar";
    case Reason::Unknown:              return "Ausente por causa desconocida";
    }
    return "Ausente por causa desconocida";
}

} // namespace pw
