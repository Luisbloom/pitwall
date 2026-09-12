#include "pw/core/channel.hpp"

namespace pw {

const char* provenanceName(const Provenance provenance) noexcept
{
    switch (provenance) {
    case Provenance::Raw:         return "raw";
    case Provenance::Derived:     return "derived";
    case Provenance::Estimated:   return "estimated";
    case Provenance::Unavailable: return "unavailable";
    case Provenance::Restricted:  return "restricted";
    }
    return "unavailable";
}

} // namespace pw
