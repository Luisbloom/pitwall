#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::proto — PacketHeader: los 29 bytes comunes a 2025 y 2026 (F1.2a-01).
//
// Es la UNICA parte del datagrama que puede leerse antes de saber de que formato
// viene, y por eso vive fuera de los espacios de nombres por formato. Su tamano y
// su disposicion son identicos en los dos (03-UDP-PROTOCOL 2).
//
// ── QUE ES ESTE FICHERO Y QUE NO ES ─────────────────────────────────────────
//
// Es una TRANSCRIPCION de la especificacion, no codigo con logica. Los campos
// conservan el nombre oficial `m_xxx` aunque contradiga el estilo del proyecto:
// la trazabilidad campo a campo con el documento de EA vale mas que la
// coherencia estilistica, y el prefijo avisa de que se esta en la capa de
// protocolo (17-PROJECT-STRUCTURE 5).
//
// Aqui NO se lee ningun datagrama. Interpretar bytes recibidos es del decoder
// (F1.2a-07 y siguientes), y hacerlo exige antes una validacion de tamano que
// este fichero no puede dar.
//
// ── LO QUE LOS static_assert DEMUESTRAN, Y LO QUE NO ────────────────────────
//
// Demuestran que ESTE struct, compilado por ESTE compilador, mide 29 B, no lleva
// relleno y coloca cada campo donde la especificacion dice. Eso descarta un error
// de transcripcion o un `#pragma pack` mal cerrado.
//
// NO dicen nada sobre un datagrama entrante. Que el struct mida 29 B no implica
// que lo recibido tenga 29 B: comprobarlo es responsabilidad del decoder, en
// tiempo de ejecucion y ANTES de mirar el contenido (06-REALTIME-PIPELINE 3).
// Son dos verificaciones distintas y ninguna sustituye a la otra.
//
// Fuente de verdad: `Data Output from F1 25: 2026 Season Pack` v1.2 (ADR-015).
// ─────────────────────────────────────────────────────────────────────────────
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace pw::proto {

/// Tipos de paquete del protocolo, tal como viajan en `m_packetId`.
///
/// Los identificadores 0..15 existen en 2025 y en 2026; el 16 -Car Telemetry 2-
/// SOLO en 2026 (03-UDP-PROTOCOL 3). Que un valor este en esta enumeracion
/// significa que el protocolo lo define, NO que el formato recibido lo emita:
/// eso lo decide la tabla de tamanos de cada formato.
enum class PacketId : std::uint8_t {
    Motion              = 0,
    Session             = 1,
    LapData             = 2,
    Event               = 3,
    Participants        = 4,
    CarSetups           = 5,
    CarTelemetry        = 6,
    CarStatus           = 7,
    FinalClassification = 8,
    LobbyInfo           = 9,
    CarDamage           = 10,
    SessionHistory      = 11,
    TyreSets            = 12,
    MotionEx            = 13,
    TimeTrial           = 14,
    LapPositions        = 15,
    CarTelemetry2       = 16,   ///< Sólo formato 2026
};

// El protocolo va PACKED y sin relleno: el tamano de un struct es la suma exacta
// de sus campos (03-UDP-PROTOCOL 1). Un solo mecanismo en todo el modulo.
#pragma pack(push, 1)

/// Cabecera comun de 29 B. Identica en 2025 y 2026.
struct PacketHeader {
    std::uint16_t m_packetFormat;             ///< 0:  2025 o 2026
    std::uint8_t  m_gameYear;                 ///< 2:  25 o 26
    std::uint8_t  m_gameMajorVersion;         ///< 3:  «X.00»
    std::uint8_t  m_gameMinorVersion;         ///< 4:  «1.XX»
    std::uint8_t  m_packetVersion;            ///< 5:  version de este tipo de paquete; empieza en 1
    std::uint8_t  m_packetId;                 ///< 6:  tipo de paquete; ver PacketId
    std::uint64_t m_sessionUID;               ///< 7:  identificador unico de sesion
    float         m_sessionTime;              ///< 15: marca de tiempo de sesion, en segundos
    std::uint32_t m_frameIdentifier;          ///< 19: fotograma. RETROCEDE tras un flashback
    std::uint32_t m_overallFrameIdentifier;   ///< 23: fotograma global. NO retrocede (RN-002)
    std::uint8_t  m_playerCarIndex;           ///< 27: indice del coche del jugador
    std::uint8_t  m_secondaryPlayerCarIndex;  ///< 28: segundo jugador; 255 si no hay
};

#pragma pack(pop)

// ── Nivel 1: tamano total ───────────────────────────────────────────────────
static_assert(sizeof(PacketHeader) == 29, "PacketHeader mide 29 B (03-UDP-PROTOCOL 2)");

// ── Nivel 4: requisitos de layout ───────────────────────────────────────────
//
// `alignof == 1` es lo que delata un `#pragma pack` mal cerrado: sin el, el
// compilador alinearia `m_sessionUID` a 8 y el struct mediria 32 B en vez de 29.
// `is_standard_layout` es ademas la precondicion formal de `offsetof`.
static_assert(alignof(PacketHeader) == 1, "empaquetado a 1: sin relleno entre campos");
static_assert(std::is_trivially_copyable_v<PacketHeader>, "copiable byte a byte");
static_assert(std::is_standard_layout_v<PacketHeader>, "layout estandar: offsetof es valido");

// ── Nivel 3: offsets oficiales, campo a campo ───────────────────────────────
//
// Se comprueban los DOCE, no una muestra: el tamano total no distingue dos campos
// intercambiados del mismo ancho, y esta cabecera la leen todos los formatos.
static_assert(offsetof(PacketHeader, m_packetFormat)            ==  0);
static_assert(offsetof(PacketHeader, m_gameYear)                ==  2);
static_assert(offsetof(PacketHeader, m_gameMajorVersion)        ==  3);
static_assert(offsetof(PacketHeader, m_gameMinorVersion)        ==  4);
static_assert(offsetof(PacketHeader, m_packetVersion)           ==  5);
static_assert(offsetof(PacketHeader, m_packetId)                ==  6);
static_assert(offsetof(PacketHeader, m_sessionUID)              ==  7);
static_assert(offsetof(PacketHeader, m_sessionTime)             == 15);
static_assert(offsetof(PacketHeader, m_frameIdentifier)         == 19);
static_assert(offsetof(PacketHeader, m_overallFrameIdentifier)  == 23);
static_assert(offsetof(PacketHeader, m_playerCarIndex)          == 27);
static_assert(offsetof(PacketHeader, m_secondaryPlayerCarIndex) == 28);

// ── Nivel 2: coherencia aritmetica ──────────────────────────────────────────
static_assert(sizeof(PacketHeader)
                  == sizeof(std::uint16_t) + 5 * sizeof(std::uint8_t) + sizeof(std::uint64_t)
                         + sizeof(float) + 2 * sizeof(std::uint32_t) + 2 * sizeof(std::uint8_t),
              "la suma de los campos es el tamano: no hay relleno escondido");

// El protocolo declara `float` de 32 bits IEEE-754 (03-UDP-PROTOCOL 1.1). Son
// DOS exigencias y hacen falta las dos: el tamano mantiene en su sitio el offset
// de todo lo que viene detras, y `is_iec559` es lo unico que dice que los bits
// significan lo que el juego cree que significan. Un `float` de 4 bytes con otra
// codificacion produciria numeros plausibles y falsos, que es el peor resultado
// posible.
static_assert(sizeof(float) == 4, "float de 32 bits, como exige el protocolo");
static_assert(std::numeric_limits<float>::is_iec559,
              "float debe seguir IEEE-754 como exige el protocolo");

} // namespace pw::proto
