// Pruebas de la capa de sockets (F1.1-04, F1.1-05, F1.1-06).
//
// Todas las pruebas enlazan el puerto 0 y leen el que concede el sistema: una
// prueba que fija un puerto falla el dia que otra cosa lo esta usando, y ademas
// no puede ejecutarse dos veces a la vez.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "pw/net/udp_socket.hpp"

using pw::net::Ipv4Endpoint;
using pw::net::kLoopbackAddress;
using pw::net::makeUdpSocket;
using pw::net::SocketErrorKind;

namespace {

[[nodiscard]] bool contains(const std::string& haystack, const std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("El socket se enlaza y dice en que puerto ha quedado",
          "[net][socket][F1.1-05][RF-002][RF-003]")
{
    auto socket = makeUdpSocket();
    REQUIRE(socket);
    REQUIRE_FALSE(socket->isOpen());

    const auto bound = socket->bind(Ipv4Endpoint{kLoopbackAddress, 0});
    REQUIRE(bound.has_value());
    REQUIRE(socket->isOpen());

    // Con puerto 0 el sistema concede uno, y hay que poder saber cual.
    REQUIRE(socket->localEndpoint().port() != 0);
    REQUIRE(socket->localEndpoint().isLoopback());

    socket->close();
    REQUIRE_FALSE(socket->isOpen());
}

TEST_CASE("Una direccion que no es de esta maquina se rechaza con explicacion",
          "[net][socket][RNF-097]")
{
    auto socket = makeUdpSocket();

    // 203.0.113.0/24 esta reservado para documentacion: no es de nadie.
    const auto bound = socket->bind(Ipv4Endpoint{{203, 0, 113, 7}, 0});

    REQUIRE_FALSE(bound.has_value());
    REQUIRE(bound.error().kind == SocketErrorKind::AddressNotAvailable);
    const auto message = bound.error().describe();
    REQUIRE(contains(message, "203.0.113.7"));
    REQUIRE(contains(message, "127.0.0.1"));   // dice que poner en su lugar
    REQUIRE(contains(message, "0.0.0.0"));
}

TEST_CASE("El puerto ocupado se detecta y el mensaje dice que hacer",
          "[net][socket][F1.1-05][RNF-097]")
{
    // Esta prueba cubre tambien el uso exclusivo del puerto, y lo cubre con la
    // implementacion real: en Windows, bind() solicita SO_EXCLUSIVEADDRUSE ANTES
    // de enlazar y falla si el sistema no lo concede. Por tanto el primer
    // REQUIRE solo pasa si la exclusividad se otorgo, y el segundo comprueba lo
    // que esa exclusividad garantiza: que nadie mas pueda quedarse el puerto.
    auto first = makeUdpSocket();
    REQUIRE(first->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());
    const auto busyPort = first->localEndpoint().port();
    REQUIRE(busyPort != 0);

    auto second = makeUdpSocket();
    const auto bound = second->bind(Ipv4Endpoint{kLoopbackAddress, busyPort});

    REQUIRE_FALSE(bound.has_value());
    REQUIRE(bound.error().kind == SocketErrorKind::AddressInUse);

    const auto message = bound.error().describe();
    REQUIRE(contains(message, std::to_string(busyPort)));
    REQUIRE(contains(message, "otro programa"));
    REQUIRE(contains(message, "[net].port"));     // la accion concreta
    REQUIRE_FALSE(second->isOpen());               // y no deja el socket a medias

    // En cuanto se libera, el mismo puerto vuelve a estar disponible.
    first->close();
    auto third = makeUdpSocket();
    REQUIRE(third->bind(Ipv4Endpoint{kLoopbackAddress, busyPort}).has_value());
}

TEST_CASE("SO_RCVBUF se solicita y se comprueba lo concedido",
          "[net][socket][F1.1-06][RNF-026]")
{
    auto socket = makeUdpSocket();
    REQUIRE(socket->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());

    const auto beforeRequest = socket->receiveBufferBytes();
    REQUIRE(beforeRequest.has_value());

    constexpr std::size_t kRequested = 4 * 1024 * 1024;   // RNF-026
    const auto granted = socket->setReceiveBufferBytes(kRequested);
    REQUIRE(granted.has_value());

    // Lo que importa de F1.1-06: el valor devuelto es el EFECTIVO, leido del
    // sistema con getsockopt, no el que se pidio. Puede ser mayor (Linux
    // duplica) o menor (un limite del sistema), pero nunca se supone.
    const auto effective = socket->receiveBufferBytes();
    REQUIRE(effective.has_value());
    REQUIRE(granted.value() == effective.value());
    REQUIRE(effective.value() >= beforeRequest.value());
    REQUIRE(effective.value() > 0);

    WARN("SO_RCVBUF por defecto: " << beforeRequest.value() << " B, solicitado: " << kRequested
                                   << " B, concedido: " << effective.value() << " B");
}

TEST_CASE("Un datagrama llega byte por byte sin alterarse", "[net][socket][RF-001]")
{
    auto receiver = makeUdpSocket();
    REQUIRE(receiver->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());
    REQUIRE(receiver->setReceivePollTimeout(2000).has_value());
    const auto target = receiver->localEndpoint();

    // Un patron con ceros, 0xFF y valores variados: si algo se trunca en un
    // cero o se reinterpreta, se nota.
    std::vector<std::byte> payload(517);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>((i * 37 + (i % 7)) & 0xFF);
    }
    payload[0]                  = std::byte{0x00};
    payload[1]                  = std::byte{0xFF};
    payload[payload.size() - 1] = std::byte{0x5A};

    auto sender = makeUdpSocket();
    REQUIRE(sender->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());
    const auto sent = sender->sendTo(target, payload);
    REQUIRE(sent.has_value());
    REQUIRE(sent.value() == payload.size());

    std::array<std::byte, 2048> buffer{};
    const auto                  received = receiver->receive(buffer);
    REQUIRE(received.has_value());
    REQUIRE(received.value().size == payload.size());

    REQUIRE(std::memcmp(buffer.data(), payload.data(), payload.size()) == 0);

    // Y el origen es quien lo envio de verdad (RF-009).
    REQUIRE(received.value().origin.isLoopback());
    REQUIRE(received.value().origin.port() == sender->localEndpoint().port());
}

TEST_CASE("Un datagrama del tamano del buffer se acepta entero", "[net][socket][RF-001]")
{
    auto receiver = makeUdpSocket();
    REQUIRE(receiver->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());
    REQUIRE(receiver->setReceivePollTimeout(2000).has_value());

    auto sender = makeUdpSocket();
    REQUIRE(sender->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());

    constexpr std::size_t kBufferBytes = 2048;   // el del BufferPool
    std::vector<std::byte> payload(kBufferBytes, std::byte{0xC3});

    REQUIRE(sender->sendTo(receiver->localEndpoint(), payload).has_value());

    std::array<std::byte, kBufferBytes> buffer{};
    const auto                         received = receiver->receive(buffer);
    REQUIRE(received.has_value());
    REQUIRE(received.value().size == kBufferBytes);
    REQUIRE(buffer[kBufferBytes - 1] == std::byte{0xC3});
}

TEST_CASE("Un datagrama mayor que el buffer se reporta, no se cuela a medias",
          "[net][socket][RNF-023]")
{
    auto receiver = makeUdpSocket();
    REQUIRE(receiver->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());
    REQUIRE(receiver->setReceivePollTimeout(2000).has_value());

    auto sender = makeUdpSocket();
    REQUIRE(sender->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());

    constexpr std::size_t kBufferBytes = 2048;
    std::vector<std::byte> payload(kBufferBytes + 1, std::byte{0x7E});
    REQUIRE(sender->sendTo(receiver->localEndpoint(), payload).has_value());

    std::array<std::byte, kBufferBytes> buffer{};
    const auto                         received = receiver->receive(buffer);

    // Lo importante es que se distingue de un datagrama valido: quien recibe no
    // puede confundir 2048 bytes de un datagrama de 2049 con uno completo.
    REQUIRE_FALSE(received.has_value());
    REQUIRE(received.error().kind == SocketErrorKind::Oversized);
}

TEST_CASE("La espera acotada expira sin datos y no es un fallo",
          "[net][socket][F1.1-07]")
{
    auto socket = makeUdpSocket();
    REQUIRE(socket->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());
    REQUIRE(socket->setReceivePollTimeout(20).has_value());

    std::array<std::byte, 64> buffer{};
    const auto                received = socket->receive(buffer);

    REQUIRE_FALSE(received.has_value());
    REQUIRE(received.error().kind == SocketErrorKind::Timeout);
    REQUIRE(socket->isOpen());   // el socket sigue perfectamente utilizable
}

TEST_CASE("Operar sobre un socket cerrado se rechaza sin caerse",
          "[net][socket][F1.1-12]")
{
    auto socket = makeUdpSocket();
    REQUIRE(socket->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());
    socket->close();

    std::array<std::byte, 64> buffer{};

    REQUIRE_FALSE(socket->receive(buffer).has_value());
    REQUIRE(socket->receive(buffer).error().kind == SocketErrorKind::Closed);
    REQUIRE_FALSE(socket->receiveBufferBytes().has_value());
    REQUIRE_FALSE(socket->setReceiveBufferBytes(1024).has_value());
    REQUIRE_FALSE(socket->setReceivePollTimeout(10).has_value());
    REQUIRE_FALSE(socket->sendTo(Ipv4Endpoint{kLoopbackAddress, 1}, buffer).has_value());

    // Cerrar dos veces es correcto y no hace nada.
    socket->close();
    REQUIRE_FALSE(socket->isOpen());
}

TEST_CASE("Reenlazar el mismo socket cierra el anterior", "[net][socket]")
{
    auto socket = makeUdpSocket();
    REQUIRE(socket->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());
    const auto firstPort = socket->localEndpoint().port();

    REQUIRE(socket->bind(Ipv4Endpoint{kLoopbackAddress, 0}).has_value());
    const auto secondPort = socket->localEndpoint().port();

    REQUIRE(secondPort != 0);
    REQUIRE(socket->isOpen());

    // El primer puerto ha quedado libre: otro socket puede tomarlo.
    auto other = makeUdpSocket();
    REQUIRE(other->bind(Ipv4Endpoint{kLoopbackAddress, firstPort}).has_value());
}
