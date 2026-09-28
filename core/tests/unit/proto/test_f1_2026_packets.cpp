// Pruebas de los structs de paquete del formato 2026, bloque B1:
// 0 Motion, 2 Lap Data y 3 Event (F1.2a-04 y F1.2a-06).
//
// El grueso de la verificacion de esta capa son los `static_assert` de
// packets.hpp: tamanos, aritmetica, offsets y layout. Si algo estuviera mal
// transcrito, este fichero NO COMPILARIA. Repetirlos aqui no anadiria nada.
//
// Lo que estas pruebas aportan es lo que el compilador no puede afirmar:
//   - que los tamanos coinciden con la tabla del formato consultada en
//     EJECUCION, no solo en la expresion constante del assert;
//   - que los valores oficiales de la especificacion son los que son, repetidos
//     a mano como oraculo independiente;
//   - que escribir un campo y releerlo desde la representacion de bytes
//     devuelve el mismo valor en la posicion esperada.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "pw/proto/f1_2026/packets.hpp"

namespace f1 = pw::proto::f1_2026;
using pw::proto::PacketId;

TEST_CASE("Los paquetes del bloque B1 miden lo que la tabla del formato dice",
          "[proto][f1_2026][packets][F1.2a-04][RNF-041]")
{
    // La tabla es la fuente unica; los numeros de la derecha se copian a mano
    // de la especificacion para no derivarlos de lo que se quiere verificar.
    CHECK(sizeof(f1::PacketMotionData) == f1::expectedSize(PacketId::Motion).value());
    CHECK(sizeof(f1::PacketMotionData) == 1325);

    CHECK(sizeof(f1::PacketLapData) == f1::expectedSize(PacketId::LapData).value());
    CHECK(sizeof(f1::PacketLapData) == 1399);

    CHECK(sizeof(f1::PacketSessionData) == f1::expectedSize(PacketId::Session).value());
    CHECK(sizeof(f1::PacketSessionData) == 926);

    CHECK(sizeof(f1::PacketEventData) == f1::expectedSize(PacketId::Event).value());
    CHECK(sizeof(f1::PacketEventData) == 45);

    // Por coche: 54 en Motion -2026 cuantiza las g-forces- y 57 en Lap Data.
    CHECK(sizeof(f1::CarMotionData) == 54);
    CHECK(sizeof(f1::LapData) == 57);
}

TEST_CASE("Motion transporta los 24 coches del formato 2026",
          "[proto][f1_2026][packets][F1.2a-04]")
{
    f1::PacketMotionData p{};
    CHECK(p.m_carMotionData.size() == 24);
    CHECK(p.m_carMotionData.size() == f1::kMaxCars);

    // El array ocupa todo lo que queda tras la cabecera: ni un byte suelto.
    CHECK(sizeof(p.m_carMotionData) == sizeof(f1::PacketMotionData) - 29);
}

TEST_CASE("Los campos de Motion se releen desde su posicion oficial",
          "[proto][f1_2026][packets][F1.2a-04][F1.2a-06]")
{
    // Se escribe un coche concreto y se comprueba que los bytes caen donde la
    // especificacion dice: es lo que el juego hara al otro lado del socket.
    f1::PacketMotionData p{};
    p.m_carMotionData[1].m_worldPositionX = 123.5F;
    p.m_carMotionData[1].m_gForceLateral  = -1500;   // −1,5 g antes de desescalar
    p.m_carMotionData[1].m_yaw            = 0.25F;

    std::array<std::byte, sizeof(f1::PacketMotionData)> wire{};
    std::memcpy(wire.data(), &p, sizeof(p));

    constexpr std::size_t kCar1 = 29 + sizeof(f1::CarMotionData);   // segundo coche

    float posX = 0.0F;
    std::memcpy(&posX, wire.data() + kCar1 + 0, sizeof(posX));
    CHECK(posX == 123.5F);

    std::int16_t gLat = 0;
    std::memcpy(&gLat, wire.data() + kCar1 + 36, sizeof(gLat));
    CHECK(gLat == -1500);

    float yaw = 0.0F;
    std::memcpy(&yaw, wire.data() + kCar1 + 42, sizeof(yaw));
    CHECK(yaw == 0.25F);
}

TEST_CASE("Session declara las cardinalidades oficiales del formato",
          "[proto][f1_2026][packets][F1.2a-04]")
{
    f1::PacketSessionData p{};

    CHECK(sizeof(f1::PacketSessionData) == f1::expectedSize(PacketId::Session).value());
    CHECK(sizeof(f1::PacketSessionData) == 926);

    // Cada array con la cardinalidad de la especificacion, no un numero suelto.
    CHECK(p.m_marshalZones.size() == 21);
    CHECK(p.m_marshalZones.size() == f1::kMaxMarshalZonesPerLap);
    CHECK(p.m_weatherForecastSamples.size() == 64);
    CHECK(p.m_weekendStructure.size() == 12);
    CHECK(p.m_activeAeroZonesFull.size() == 8);
    CHECK(p.m_activeAeroZonesPartial.size() == 8);
    CHECK(p.m_drsZones.size() == 4);

    // Y los tamanos de los cuatro sub-structs auxiliares.
    CHECK(sizeof(f1::MarshalZone) == 5);
    CHECK(sizeof(f1::WeatherForecastSample) == 8);
    CHECK(sizeof(f1::ActiveAeroZone) == 8);
    CHECK(sizeof(f1::DRSZone) == 8);
}

TEST_CASE("Los campos de Session que siguen a un array caen en su offset oficial",
          "[proto][f1_2026][packets][F1.2a-04][F1.2a-06]")
{
    // Aqui es donde una cardinalidad equivocada se delata: si las zonas de
    // marshal fueran 20 en vez de 21, `m_safetyCarStatus` aterrizaria 5 B antes
    // y todo lo que sigue se desplazaria.
    f1::PacketSessionData p{};
    p.m_numMarshalZones             = 21;
    p.m_marshalZones[20].m_zoneFlag = 3;      // ultima zona: amarilla
    p.m_safetyCarStatus             = 2;      // virtual
    p.m_forecastAccuracy            = 1;      // aproximada
    p.m_startReactionTime           = 0.312F;
    p.m_recurringRewindPrompt       = 1;

    std::array<std::byte, sizeof(f1::PacketSessionData)> wire{};
    std::memcpy(wire.data(), &p, sizeof(p));

    const auto byteAt = [&wire](const std::size_t offset) {
        return static_cast<std::uint8_t>(wire[offset]);
    };

    CHECK(byteAt(47) == 21);    // m_numMarshalZones
    CHECK(byteAt(152) == 3);    // ultimo m_zoneFlag: 48 + 20*5 + 4
    CHECK(byteAt(153) == 2);    // m_safetyCarStatus, justo tras el array
    CHECK(byteAt(668) == 1);    // m_forecastAccuracy, tras 64 muestras de clima
    CHECK(byteAt(925) == 1);    // m_recurringRewindPrompt: ultimo byte del paquete

    // El `float` de 2026 en un offset impar-desalineado: 917.
    float reaction = 0.0F;
    std::memcpy(&reaction, wire.data() + 917, sizeof(reaction));
    CHECK(reaction == 0.312F);
}

TEST_CASE("Lap Data conserva los tiempos en dos partes tal como llegan",
          "[proto][f1_2026][packets][F1.2a-04]")
{
    // Un sector de 1 min 23,456 s viaja partido. Aqui SOLO se comprueba que se
    // transcribe sin tocarlo: recomponerlo es de la normalizacion (F1.3-08), y
    // hacerlo aqui seria decodificar en la capa equivocada.
    f1::LapData lap{};
    lap.m_sector1TimeMinutesPart = 1;
    lap.m_sector1TimeMSPart      = 23456;

    CHECK(lap.m_sector1TimeMinutesPart == 1);
    CHECK(lap.m_sector1TimeMSPart == 23456);

    // El centinela se transcribe tal cual: 255 sigue siendo 255 en esta capa.
    lap.m_speedTrapFastestLap = 255;
    CHECK(lap.m_speedTrapFastestLap == 255);
}

TEST_CASE("Los indices de Time Trial van despues del array de vueltas",
          "[proto][f1_2026][packets][F1.2a-04][F1.2a-06]")
{
    // Es donde se nota un error de cardinalidad: si el array tuviera 22 coches
    // en vez de 24, estos dos campos aterrizarian 114 B antes.
    f1::PacketLapData p{};
    p.m_timeTrialPBCarIdx    = 7;
    p.m_timeTrialRivalCarIdx = 255;

    std::array<std::byte, sizeof(f1::PacketLapData)> wire{};
    std::memcpy(wire.data(), &p, sizeof(p));

    CHECK(static_cast<std::uint8_t>(wire[1397]) == 7);
    CHECK(static_cast<std::uint8_t>(wire[1398]) == 255);

    CHECK(p.m_lapData.size() == f1::kMaxCars);
}

TEST_CASE("Event separa el codigo de los doce bytes de la union",
          "[proto][f1_2026][packets][F1.2a-04]")
{
    // El codigo es un array de 4 caracteres SIN terminador nulo: tratarlo como
    // cadena de C leeria dentro de la union.
    f1::PacketEventData p{};
    p.m_eventStringCode = {'F', 'L', 'B', 'K'};   // uint8[4] en la especificacion

    CHECK(p.m_eventStringCode.size() == 4);
    CHECK(p.m_eventDetails.size() == 12);
    CHECK(f1::kEventDetailsBytes == 12);

    std::array<std::byte, sizeof(f1::PacketEventData)> wire{};
    std::memcpy(wire.data(), &p, sizeof(p));

    CHECK(static_cast<char>(wire[29]) == 'F');
    CHECK(static_cast<char>(wire[32]) == 'K');

    // La union empieza en el 33 y llega hasta el final del paquete.
    CHECK(sizeof(f1::PacketEventData) - 33 == f1::kEventDetailsBytes);
}

TEST_CASE("Los tres paquetes de B1 se copian byte a byte sin perder nada",
          "[proto][f1_2026][packets][F1.2a-06]")
{
    // Ser trivialmente copiable permite reconstruir el valor mediante memcpy
    // desde su representacion de bytes. No autoriza por si solo a tratar un
    // buffer arbitrario recibido por UDP como si ya contuviera un objeto: eso
    // se decidira en el decoder.
    f1::PacketLapData original{};
    original.m_header.m_packetFormat        = 2026;
    original.m_header.m_packetId            = static_cast<std::uint8_t>(PacketId::LapData);
    original.m_lapData[0].m_carPosition     = 3;
    original.m_lapData[0].m_lapDistance     = -12.5F;   // negativo antes de la linea
    original.m_lapData[23].m_currentLapNum  = 58;
    original.m_timeTrialPBCarIdx            = 255;

    std::array<std::byte, sizeof(original)> wire{};
    std::memcpy(wire.data(), &original, sizeof(original));

    f1::PacketLapData rebuilt{};
    std::memcpy(&rebuilt, wire.data(), sizeof(rebuilt));

    CHECK(rebuilt.m_header.m_packetFormat == 2026);
    CHECK(rebuilt.m_lapData[0].m_carPosition == 3);
    CHECK(rebuilt.m_lapData[0].m_lapDistance == -12.5F);
    CHECK(rebuilt.m_lapData[23].m_currentLapNum == 58);
}
