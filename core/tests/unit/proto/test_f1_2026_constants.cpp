// Pruebas de las constantes y la tabla de tamanos del formato 2026
// (F1.2a-02 y F1.2a-03).
//
// Los numeros de este fichero son una TRANSCRIPCION de la especificacion, y por
// eso se comprueban uno a uno en vez de derivarlos: derivarlos de la misma
// constante que se quiere verificar no probaria nada. La fuente es
// `Data Output from F1 25: 2026 Season Pack` v1.2 (ADR-015), recogida en
// 03-UDP-PROTOCOL 1.2 y 3.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "pw/proto/f1_2026/constants.hpp"

namespace f1 = pw::proto::f1_2026;
using pw::proto::PacketId;

TEST_CASE("Las constantes del formato 2026 valen lo que dice la especificacion",
          "[proto][f1_2026][F1.2a-02]")
{
    CHECK(f1::kPacketFormat == 2026);

    // 24 coches, no 22: es la diferencia con 2025 que arrastra el tamano de casi
    // todos los paquetes.
    CHECK(f1::kMaxCars == 24);

    CHECK(f1::kParticipantNameLength == 32);
    CHECK(f1::kMaxTyreStints == 8);
    CHECK(f1::kMaxTyreSets == 20);
    CHECK(f1::kMaxMarshalZonesPerLap == 21);
    CHECK(f1::kMaxWeatherForecastSamples == 64);
    CHECK(f1::kMaxSessionsInWeekend == 12);
    CHECK(f1::kMaxLapsInHistory == 100);
    CHECK(f1::kMaxLapsInLapPositionsHistory == 50);

    // Las dos que no existen en 2025.
    CHECK(f1::kMaxActiveAeroZonesPerLap == 8);
    CHECK(f1::kMaxDrsZones == 4);
}

TEST_CASE("Los 17 paquetes del formato 2026 tienen su tamano oficial",
          "[proto][f1_2026][F1.2a-03][RNF-041]")
{
    CHECK(f1::kPacketCount == 17);

    CHECK(f1::expectedSize(PacketId::Motion).value()              == 1325);
    CHECK(f1::expectedSize(PacketId::Session).value()             ==  926);
    CHECK(f1::expectedSize(PacketId::LapData).value()             == 1399);
    CHECK(f1::expectedSize(PacketId::Event).value()               ==   45);
    CHECK(f1::expectedSize(PacketId::Participants).value()        == 1470);
    CHECK(f1::expectedSize(PacketId::CarSetups).value()           == 1233);
    CHECK(f1::expectedSize(PacketId::CarTelemetry).value()        == 1448);
    CHECK(f1::expectedSize(PacketId::CarStatus).value()           == 1445);
    CHECK(f1::expectedSize(PacketId::FinalClassification).value() == 1134);
    CHECK(f1::expectedSize(PacketId::LobbyInfo).value()           == 1062);
    CHECK(f1::expectedSize(PacketId::CarDamage).value()           == 1133);
    CHECK(f1::expectedSize(PacketId::SessionHistory).value()      == 1460);
    CHECK(f1::expectedSize(PacketId::TyreSets).value()            ==  231);
    CHECK(f1::expectedSize(PacketId::MotionEx).value()            ==  273);
    CHECK(f1::expectedSize(PacketId::TimeTrial).value()           ==  104);
    CHECK(f1::expectedSize(PacketId::LapPositions).value()        == 1231);
    CHECK(f1::expectedSize(PacketId::CarTelemetry2).value()       ==  269);
}

TEST_CASE("Un identificador que el formato no define no tiene tamano",
          "[proto][f1_2026][F1.2a-03][SEC-04]")
{
    // `m_packetId` llega de la red y puede traer cualquier valor de 8 bits. La
    // respuesta correcta es «no se», no un numero: devolver 0 haria pasar por
    // valido un datagrama vacio, y un valor cualquiera haria leer fuera de la
    // tabla.
    CHECK_FALSE(f1::isKnownPacketId(static_cast<PacketId>(17)));
    CHECK_FALSE(f1::expectedSize(static_cast<PacketId>(17)).has_value());

    CHECK_FALSE(f1::isKnownPacketId(static_cast<PacketId>(255)));
    CHECK_FALSE(f1::expectedSize(static_cast<PacketId>(255)).has_value());

    // Los dos extremos del rango valido si existen.
    CHECK(f1::isKnownPacketId(PacketId::Motion));
    CHECK(f1::isKnownPacketId(PacketId::CarTelemetry2));
}

TEST_CASE("La tabla de tamanos se puede consultar en tiempo de compilacion",
          "[proto][f1_2026][F1.2a-03][RNF-041]")
{
    // No es un adorno: los `static_assert` de los structs de F1.2a-04 y 05
    // compararan su `sizeof` contra esta tabla, y para eso la consulta tiene que
    // ser utilizable en una expresion constante.
    static_assert(f1::expectedSize(PacketId::CarTelemetry).value() == 1448);
    static_assert(!f1::expectedSize(static_cast<PacketId>(200)).has_value());
    static_assert(f1::isKnownPacketId(PacketId::MotionEx));

    SUCCEED("la tabla es consultable en constexpr");
}

TEST_CASE("Ningun tamano de paquete es menor que la cabecera comun",
          "[proto][f1_2026][F1.2a-03]")
{
    // Todo paquete del protocolo empieza por los 29 B de cabecera. Un tamano
    // menor solo podria venir de un error de transcripcion.
    for (std::size_t id = 0; id < f1::kPacketCount; ++id) {
        INFO("packetId = " << id);
        CHECK(f1::kPacketSizes[id] >= sizeof(pw::proto::PacketHeader));
    }
}
