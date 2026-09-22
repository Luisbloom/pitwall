#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Socket de prueba que entrega un guion fijo de datagramas: (origen, tamano).
//
// Existe para las pruebas de la tabla de origenes. Con sockets reales en
// loopback solo se puede variar el PUERTO del emisor, no su IP, y para llenar
// la tabla o probar «misma IP, otro puerto» frente a «otra IP, mismo puerto»
// hace falta elegir las dos. El receptor acepta el socket por constructor, asi
// que se entra por la costura que ya existia.
//
// No asigna en receive(): si lo hiciera, las pruebas de asignaciones medirian
// el guion en vez del receptor. El guion se monta entero antes de arrancar.
// ─────────────────────────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>

#include "pw/net/udp_socket.hpp"

namespace pw::test {

struct ScriptedDatagram {
    net::Ipv4Endpoint origin{};
    std::size_t       size{0};
    /// Si es true, esta entrada no entrega un datagrama sino el error `error`.
    bool                 injectError{false};
    net::SocketErrorKind error{net::SocketErrorKind::Unknown};
};

/// Entrada de guion que devuelve un error del socket en vez de un datagrama.
[[nodiscard]] constexpr ScriptedDatagram scriptedError(const net::SocketErrorKind kind) noexcept
{
    return ScriptedDatagram{{}, 0, true, kind};
}

class ScriptedSocket final : public net::IUdpSocket {
public:
    static constexpr std::size_t kMaxEntries = 64;

    /// Funcion opcional que se consulta al principio de cada receive(). Las
    /// pruebas de asignaciones la usan para saber cuantas llevaba T1 al empezar
    /// a recibir cada datagrama, y asi atribuirlas una a una.
    using Probe = std::uint64_t (*)() noexcept;

    /// Entrega `entries` en orden, `repeats` veces, y despues solo esperas
    /// vencidas.
    ScriptedSocket(std::span<const ScriptedDatagram> entries, const std::size_t repeats = 1,
                   const Probe probe = nullptr) noexcept
        : count_{std::min(entries.size(), kMaxEntries)}, repeats_{repeats}, probe_{probe}
    {
        std::copy_n(entries.begin(), count_, entries_.begin());
    }

    Result<void, net::SocketError> bind(const net::Ipv4Endpoint&) override { return {}; }
    Result<std::size_t, net::SocketError> setReceiveBufferBytes(const std::size_t n) override
    {
        return n;
    }
    Result<std::size_t, net::SocketError> receiveBufferBytes() const override
    {
        return std::size_t{0};
    }
    Result<void, net::SocketError> setReceivePollTimeout(std::uint32_t) override { return {}; }

    Result<net::ReceivedBytes, net::ReceiveError> receive(const std::span<std::byte> buffer) override
    {
        const auto position = next_.load(std::memory_order_relaxed);
        if (probe_ != nullptr && position < probes_.size()) { probes_[position] = probe_(); }

        if (count_ == 0 || position >= count_ * repeats_
            || position >= allowed_.load(std::memory_order_acquire)) {
            // Fin del guion, o del tramo permitido: un socket en silencio.
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            idlePolls_.fetch_add(1, std::memory_order_release);
            return Unexpected<net::ReceiveError>{net::ReceiveError{net::SocketErrorKind::Timeout, 0}};
        }

        const auto& entry = entries_[position % count_];
        next_.store(position + 1, std::memory_order_release);

        if (entry.injectError) {
            return Unexpected<net::ReceiveError>{net::ReceiveError{entry.error, 0}};
        }
        if (entry.size > buffer.size()) {
            return Unexpected<net::ReceiveError>{net::ReceiveError{net::SocketErrorKind::Oversized, 0}};
        }
        std::memset(buffer.data(), 0x5A, entry.size);
        return net::ReceivedBytes{entry.size, entry.origin};
    }

    net::Ipv4Endpoint localEndpoint() const noexcept override { return {}; }
    Result<std::size_t, net::SocketError> sendTo(const net::Ipv4Endpoint&,
                                                 std::span<const std::byte>) override
    {
        return std::size_t{0};
    }
    void close() noexcept override {}
    bool isOpen() const noexcept override { return true; }

    /// Datagramas del guion ya entregados al receptor.
    [[nodiscard]] std::size_t delivered() const noexcept
    {
        return next_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t total() const noexcept { return count_ * repeats_; }

    /// Entrega como mucho `n` datagramas en total hasta nueva orden. Sirve para
    /// separar calentamiento y medicion: sin esto, un guion sin esperas se
    /// consumiria entero antes de que la prueba pudiera parar el bucle.
    void allowUpTo(const std::size_t n) noexcept { allowed_.store(n, std::memory_order_release); }

    /// Esperas vencidas devueltas: cada una es una vuelta completa del bucle del
    /// receptor sin datagrama. Las pruebas esperan a que avance para saber que el
    /// receptor ya ha mirado el reloj despues de moverlo.
    [[nodiscard]] std::uint64_t idlePolls() const noexcept
    {
        return idlePolls_.load(std::memory_order_acquire);
    }

    /// Valor de la sonda al empezar la llamada `i` a receive(). Solo es fiable
    /// despues de parar el hilo del receptor.
    [[nodiscard]] std::uint64_t probeAt(const std::size_t i) const noexcept
    {
        return i < probes_.size() ? probes_[i] : 0;
    }

private:
    std::array<ScriptedDatagram, kMaxEntries> entries_{};
    std::size_t                               count_;
    std::size_t                               repeats_;
    Probe                                     probe_;
    std::array<std::uint64_t, kMaxEntries + 1> probes_{};
    std::atomic<std::size_t>                  next_{0};
    std::atomic<std::size_t>                  allowed_{static_cast<std::size_t>(-1)};
    std::atomic<std::uint64_t>                idlePolls_{0};
};

} // namespace pw::test
