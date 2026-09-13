#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::net::Ipv4Endpoint — direccion y puerto (F1.1-04).
//
// Solo IPv4: el juego emite IPv4 (06-REALTIME-PIPELINE.md 2.1). No se abstrae
// para IPv6 porque no hay ningun caso que lo pida; el dia que lo haya, se anade.
//
// Es un tipo trivial y sin asignaciones: viaja en las colas del camino caliente
// junto al handle del buffer, y recvfrom lo rellena en cada datagrama.
// ─────────────────────────────────────────────────────────────────────────────
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace pw::net {

class Ipv4Endpoint {
public:
    constexpr Ipv4Endpoint() noexcept = default;
    constexpr Ipv4Endpoint(std::array<std::uint8_t, 4> address, std::uint16_t port) noexcept
        : address_{address}, port_{port}
    {
    }

    /// Analiza «a.b.c.d». Devuelve false si no lo es: no resuelve nombres, no
    /// consulta el DNS y no toca la red.
    [[nodiscard]] static bool parse(std::string_view text, std::uint16_t port,
                                    Ipv4Endpoint& out) noexcept;

    [[nodiscard]] constexpr std::array<std::uint8_t, 4> address() const noexcept { return address_; }
    [[nodiscard]] constexpr std::uint16_t               port()    const noexcept { return port_; }

    /// Entero en orden de red (big endian), tal y como lo quiere sockaddr_in.
    [[nodiscard]] constexpr std::uint32_t toNetworkOrder() const noexcept
    {
        return (static_cast<std::uint32_t>(address_[0]) << 24)
               | (static_cast<std::uint32_t>(address_[1]) << 16)
               | (static_cast<std::uint32_t>(address_[2]) << 8)
               | static_cast<std::uint32_t>(address_[3]);
    }

    [[nodiscard]] static constexpr Ipv4Endpoint fromNetworkOrder(std::uint32_t address,
                                                                 std::uint16_t port) noexcept
    {
        return Ipv4Endpoint{{static_cast<std::uint8_t>((address >> 24) & 0xFFu),
                             static_cast<std::uint8_t>((address >> 16) & 0xFFu),
                             static_cast<std::uint8_t>((address >> 8) & 0xFFu),
                             static_cast<std::uint8_t>(address & 0xFFu)},
                            port};
    }

    /// «127.0.0.1:20777». Asigna: NO se usa en el camino caliente.
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] constexpr bool isAny()      const noexcept { return toNetworkOrder() == 0; }
    [[nodiscard]] constexpr bool isLoopback() const noexcept { return address_[0] == 127; }

    constexpr bool operator==(const Ipv4Endpoint&) const noexcept = default;

private:
    std::array<std::uint8_t, 4> address_{};
    std::uint16_t               port_{0};
};

static_assert(std::is_trivially_copyable_v<Ipv4Endpoint>,
              "Ipv4Endpoint viaja por colas sin bloqueos: debe ser trivialmente copiable");

/// Las dos direcciones con nombre que aparecen en la configuracion.
inline constexpr std::array<std::uint8_t, 4> kLoopbackAddress{127, 0, 0, 1};
inline constexpr std::array<std::uint8_t, 4> kAnyAddress{0, 0, 0, 0};

} // namespace pw::net
