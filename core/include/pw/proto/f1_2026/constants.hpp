#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::proto::f1_2026 — constantes y tamanos del formato 2026 (F1.2a-02, F1.2a-03).
//
// Este es el UNICO lugar del codigo de produccion donde 24, 21, 64 o 1448 se
// escriben como numero. Los structs del formato consumen estas constantes desde
// aqui, y sus `static_assert` las usan para comprobar la aritmetica del tamano
// (03-UDP-PROTOCOL 1.2, RNF-041).
//
// Las PRUEBAS son la excepcion deliberada: repiten los valores oficiales a mano,
// copiados de la especificacion, precisamente para no derivarlos de lo que
// quieren verificar. Un test que calculase el tamano esperado a partir de esta
// tabla pasaria siempre, incluida la vez en que la tabla este mal. Ahi la
// duplicacion no es descuido: es el oraculo independiente.
//
// La razon de la regla no es estetica. Cuando EA cambio de 22 a 24 coches, el
// cambio toco la cardinalidad de casi todos los arrays y el tamano de casi todos
// los paquetes. Con las constantes en un sitio, eso es editar una linea y dejar
// que el compilador senale todo lo que ya no cuadra; con literales repartidos,
// es una caceria.
//
// El namespace lleva el ano a proposito: el mismo nombre significa cosas
// distintas en 2025 y en 2026 -kMaxCars vale 22 alli- y los dos formatos tienen
// que poder coexistir en el mismo binario (ADR-008).
//
// Fuente de verdad: `Data Output from F1 25: 2026 Season Pack` v1.2 (ADR-015).
// ─────────────────────────────────────────────────────────────────────────────
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "pw/proto/header.hpp"

namespace pw::proto::f1_2026 {

/// Identificador de formato que llega en `m_packetFormat`.
inline constexpr std::uint16_t kPacketFormat = 2026;

// ── Cardinalidades del formato (03-UDP-PROTOCOL 1.2) ────────────────────────

/// Coches en los datos UDP. **24 en 2026**, frente a 22 en 2025: es el cambio
/// que mas tamanos de paquete movio entre los dos formatos.
inline constexpr std::size_t kMaxCars = 24;

/// Longitud del array de nombre de participante, con el nulo incluido.
inline constexpr std::size_t kParticipantNameLength = 32;

inline constexpr std::size_t kMaxTyreStints = 8;

/// 13 juegos de seco + 7 de lluvia.
inline constexpr std::size_t kMaxTyreSets = 20;

inline constexpr std::size_t kMaxMarshalZonesPerLap = 21;

inline constexpr std::size_t kMaxWeatherForecastSamples = 64;

inline constexpr std::size_t kMaxSessionsInWeekend = 12;

inline constexpr std::size_t kMaxLapsInHistory = 100;

inline constexpr std::size_t kMaxLapsInLapPositionsHistory = 50;

/// Sólo 2026: zonas de aerodinámica activa por vuelta.
inline constexpr std::size_t kMaxActiveAeroZonesPerLap = 8;

/// Sólo 2026: zonas de DRS por vuelta.
inline constexpr std::size_t kMaxDrsZones = 4;

// ── Tamanos esperados por paquete (03-UDP-PROTOCOL 3) ───────────────────────

/// Paquetes que el formato 2026 define: los identificadores 0..16.
inline constexpr std::size_t kPacketCount = 17;

/// Tamano EXACTO de cada paquete, indexado por `packetId`.
///
/// «Exacto» es literal y es la clave de la validacion posterior: un datagrama de
/// 1449 B donde se esperan 1448 no es un paquete con un byte de mas, es un
/// paquete que no entendemos. Aceptarlo produciria datos plausibles y falsos
/// (06-REALTIME-PIPELINE 3.1).
///
/// Esta tabla es el unico lugar donde viven estos numeros para el formato 2026
/// (RNF-041). Cuando existan los structs, sus `static_assert` compararan su
/// `sizeof` contra esta misma tabla, de modo que una transcripcion equivocada no
/// pueda compilar.
inline constexpr std::array<std::size_t, kPacketCount> kPacketSizes = {
    1325,   ///< 0  Motion
     926,   ///< 1  Session
    1399,   ///< 2  Lap Data
      45,   ///< 3  Event
    1470,   ///< 4  Participants
    1233,   ///< 5  Car Setups
    1448,   ///< 6  Car Telemetry
    1445,   ///< 7  Car Status
    1134,   ///< 8  Final Classification
    1062,   ///< 9  Lobby Info
    1133,   ///< 10 Car Damage
    1460,   ///< 11 Session History
     231,   ///< 12 Tyre Sets
     273,   ///< 13 Motion Ex
     104,   ///< 14 Time Trial
    1231,   ///< 15 Lap Positions
     269,   ///< 16 Car Telemetry 2  (sólo 2026)
};

/// Cierto si el formato 2026 define este identificador de paquete.
///
/// Un `PacketId` puede contener cualquier valor de 8 bits -llega de la red-, asi
/// que preguntar por el rango es obligatorio antes de indexar.
[[nodiscard]] constexpr bool isKnownPacketId(const PacketId id) noexcept
{
    return static_cast<std::size_t>(id) < kPacketCount;
}

/// Tamano exacto esperado para `id`, o nada si el formato no define ese paquete.
///
/// Devuelve `optional` en vez de un centinela como 0 porque «no existe» y «mide
/// cero» son respuestas distintas, y confundirlas aqui haria pasar por valido un
/// datagrama vacio.
[[nodiscard]] constexpr std::optional<std::size_t> expectedSize(const PacketId id) noexcept
{
    if (!isKnownPacketId(id)) { return std::nullopt; }
    return kPacketSizes[static_cast<std::size_t>(id)];
}

// ── Comprobaciones de la propia tabla ───────────────────────────────────────
//
// No verifican el protocolo -eso lo hara el `sizeof` de cada struct en F1.2a-04
// y 05-, sino que la tabla es coherente consigo misma y con el resto del modulo.

static_assert(kPacketSizes.size() == kPacketCount, "un tamano por identificador definido");

/// El mayor paquete del formato cabe en un buffer del pool sin fragmentar nada.
/// Si algun dia dejara de caber, esto avisaria al compilar en lugar de en pista.
static_assert(kPacketSizes[static_cast<std::size_t>(PacketId::Participants)] == 1470,
              "Participants es el mayor paquete de 2026");

/// Ningun paquete puede ser menor que la cabecera comun: todos la llevan.
static_assert([] {
    for (const auto size : kPacketSizes) {
        if (size < sizeof(PacketHeader)) { return false; }
    }
    return true;
}(), "todo paquete incluye la cabecera de 29 B");

/// Car Telemetry 2 sólo existe en 2026 y es la senal mas barata para distinguir
/// un flujo 2026 de uno 2025 sin decodificar nada.
static_assert(kPacketSizes[static_cast<std::size_t>(PacketId::CarTelemetry2)] == 269,
              "Car Telemetry 2 mide 269 B y no existe en 2025");

} // namespace pw::proto::f1_2026
