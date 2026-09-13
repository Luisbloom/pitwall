#include "pw/net/endpoint.hpp"

#include <array>
#include <cstdio>

namespace pw::net {

bool Ipv4Endpoint::parse(const std::string_view text, const std::uint16_t port,
                         Ipv4Endpoint& out) noexcept
{
    std::array<std::uint8_t, 4> octets{};
    std::size_t                 octet    = 0;
    int                         value    = -1;   // -1 = todavia ningun digito
    std::size_t                 digits   = 0;

    for (const char c : text) {
        if (c == '.') {
            if (value < 0 || octet >= 3) { return false; }
            octets[octet++] = static_cast<std::uint8_t>(value);
            value           = -1;
            digits          = 0;
            continue;
        }
        if (c < '0' || c > '9') { return false; }
        if (++digits > 3) { return false; }
        value = (value < 0 ? 0 : value) * 10 + (c - '0');
        if (value > 255) { return false; }
    }

    if (value < 0 || octet != 3) { return false; }
    octets[3] = static_cast<std::uint8_t>(value);

    out = Ipv4Endpoint{octets, port};
    return true;
}

std::string Ipv4Endpoint::toString() const
{
    std::array<char, 32> buffer{};
    const auto           written = std::snprintf(buffer.data(), buffer.size(), "%u.%u.%u.%u:%u",
                                                 static_cast<unsigned>(address_[0]),
                                                 static_cast<unsigned>(address_[1]),
                                                 static_cast<unsigned>(address_[2]),
                                                 static_cast<unsigned>(address_[3]),
                                                 static_cast<unsigned>(port_));
    return written > 0 ? std::string{buffer.data(), static_cast<std::size_t>(written)}
                       : std::string{};
}

} // namespace pw::net
