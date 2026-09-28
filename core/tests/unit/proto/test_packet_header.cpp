// Pruebas de la cabecera comun del protocolo (F1.2a-01).
//
// La mayor parte de lo que hay que demostrar sobre un struct transcrito -tamano,
// offsets, empaquetado- ya lo demuestran los `static_assert` de header.hpp: si
// algo de eso fallara, este fichero NO COMPILARIA. Repetirlos aqui como
// REQUIRE(sizeof(...) == 29) no anadiria nada; seria comprobar dos veces lo
// mismo, una de ellas mas tarde y peor.
//
// Lo que si aporta una prueba en ejecucion es lo que el compilador no puede
// afirmar por si solo: que los bytes caen donde la especificacion dice cuando se
// escriben de verdad, y que el tipo se comporta como un agregado copiable.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "pw/proto/header.hpp"

using pw::proto::PacketHeader;
using pw::proto::PacketId;

TEST_CASE("La cabecera ocupa exactamente los 29 bytes que el protocolo reserva",
          "[proto][header][F1.2a-01][RF-100]")
{
    // El `static_assert` ya lo garantiza; esto fija el numero por escrito en la
    // suite para que un cambio accidental se lea como un fallo de prueba y no
    // solo como un error de compilacion en un fichero de cabeceras.
    REQUIRE(sizeof(PacketHeader) == 29);
    REQUIRE(alignof(PacketHeader) == 1);
}

TEST_CASE("Cada campo aterriza en su offset oficial al escribir la cabecera",
          "[proto][header][F1.2a-01][RF-100]")
{
    // Se construye la cabecera por sus campos y se mira el resultado BYTE A
    // BYTE, que es justo lo que hara el juego al otro lado del socket.
    PacketHeader h{};
    h.m_packetFormat            = 2026;
    h.m_gameYear                = 26;
    h.m_gameMajorVersion        = 1;
    h.m_gameMinorVersion        = 12;
    h.m_packetVersion           = 1;
    h.m_packetId                = static_cast<std::uint8_t>(PacketId::CarTelemetry2);
    h.m_sessionUID              = 0x0123456789ABCDEFULL;
    h.m_sessionTime             = 0.0F;
    h.m_frameIdentifier         = 0x11223344U;
    h.m_overallFrameIdentifier  = 0x55667788U;
    h.m_playerCarIndex          = 19;
    h.m_secondaryPlayerCarIndex = 255;

    std::array<std::byte, sizeof(PacketHeader)> bytes{};
    std::memcpy(bytes.data(), &h, sizeof(h));

    const auto at = [&bytes](const std::size_t offset) {
        return static_cast<std::uint8_t>(bytes[offset]);
    };

    // Little endian, sin excepciones (03-UDP-PROTOCOL 1).
    CHECK(at(0) == 0xEA);   // 2026 = 0x07EA
    CHECK(at(1) == 0x07);
    CHECK(at(2) == 26);
    CHECK(at(3) == 1);
    CHECK(at(4) == 12);
    CHECK(at(5) == 1);
    CHECK(at(6) == 16);     // Car Telemetry 2

    // El uint64 empieza en 7 sin que el compilador lo haya alineado a 8: es la
    // comprobacion que delataria un empaquetado roto en una plataforma nueva.
    CHECK(at(7)  == 0xEF);
    CHECK(at(14) == 0x01);

    CHECK(at(19) == 0x44);  // m_frameIdentifier, byte bajo primero
    CHECK(at(22) == 0x11);
    CHECK(at(23) == 0x88);  // m_overallFrameIdentifier
    CHECK(at(26) == 0x55);
    CHECK(at(27) == 19);
    CHECK(at(28) == 255);
}

TEST_CASE("La cabecera se copia byte a byte sin perder ningun campo",
          "[proto][header][F1.2a-01]")
{
    // Ser trivialmente copiable permite reconstruir el valor mediante memcpy
    // desde su representacion de bytes, que es lo que se comprueba aqui. NO
    // autoriza por si solo a tratar un buffer arbitrario recibido por UDP como
    // si ya contuviera un objeto vivo de este tipo: eso es otra discusion, la
    // del acceso sin copia, y se decide cuando llegue el decoder.
    PacketHeader original{};
    original.m_packetFormat           = 2026;
    original.m_sessionUID             = 0xDEADBEEFCAFEBABEULL;
    original.m_sessionTime            = 123.456F;
    original.m_overallFrameIdentifier = 987654321U;

    std::array<std::byte, sizeof(PacketHeader)> wire{};
    std::memcpy(wire.data(), &original, sizeof(original));

    PacketHeader rebuilt{};
    std::memcpy(&rebuilt, wire.data(), sizeof(rebuilt));

    CHECK(rebuilt.m_packetFormat == 2026);
    CHECK(rebuilt.m_sessionUID == 0xDEADBEEFCAFEBABEULL);
    CHECK(rebuilt.m_sessionTime == 123.456F);
    CHECK(rebuilt.m_overallFrameIdentifier == 987654321U);
}

TEST_CASE("Los identificadores de paquete conservan su valor oficial",
          "[proto][header][F1.2a-01][RF-100]")
{
    // El valor numerico NO es un detalle interno: viaja en el datagrama. Un
    // reordenamiento accidental del enum cambiaria el significado de cada byte 6.
    CHECK(static_cast<std::uint8_t>(PacketId::Motion) == 0);
    CHECK(static_cast<std::uint8_t>(PacketId::Session) == 1);
    CHECK(static_cast<std::uint8_t>(PacketId::LapData) == 2);
    CHECK(static_cast<std::uint8_t>(PacketId::Event) == 3);
    CHECK(static_cast<std::uint8_t>(PacketId::Participants) == 4);
    CHECK(static_cast<std::uint8_t>(PacketId::CarSetups) == 5);
    CHECK(static_cast<std::uint8_t>(PacketId::CarTelemetry) == 6);
    CHECK(static_cast<std::uint8_t>(PacketId::CarStatus) == 7);
    CHECK(static_cast<std::uint8_t>(PacketId::FinalClassification) == 8);
    CHECK(static_cast<std::uint8_t>(PacketId::LobbyInfo) == 9);
    CHECK(static_cast<std::uint8_t>(PacketId::CarDamage) == 10);
    CHECK(static_cast<std::uint8_t>(PacketId::SessionHistory) == 11);
    CHECK(static_cast<std::uint8_t>(PacketId::TyreSets) == 12);
    CHECK(static_cast<std::uint8_t>(PacketId::MotionEx) == 13);
    CHECK(static_cast<std::uint8_t>(PacketId::TimeTrial) == 14);
    CHECK(static_cast<std::uint8_t>(PacketId::LapPositions) == 15);
    CHECK(static_cast<std::uint8_t>(PacketId::CarTelemetry2) == 16);
}
