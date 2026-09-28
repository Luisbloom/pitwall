#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::proto::f1_2026 — structs de los paquetes del formato 2026 (F1.2a-04/05).
//
// TRANSCRIPCION de la especificacion, no diseno. Los campos llevan el nombre
// oficial `m_xxx` y van en el orden oficial: aqui la fidelidad byte a byte vale
// mas que cualquier criterio de estilo, porque el compilador va a interpretar
// literalmente lo que se escriba en este fichero sobre bytes que vienen de la
// red.
//
// Fuente de verdad: `spec/Data Output from F1 25 2026 Season Pack (Season 8).pdf`,
// **Version 1.2** (ADR-015), contrastado campo a campo con
// `spec/2026 Season Pack Telemetry Output Structures (1).txt`. Las dos fuentes
// oficiales coinciden en nombres, tipos y ORDEN para todo lo que hay aqui; la
// unica diferencia es que el PDF escribe las cardinalidades como literales y el
// .txt usa sus constantes con nombre.
//
// ── LO QUE ESTE FICHERO NO HACE ─────────────────────────────────────────────
//
// No lee datagramas, no valida tamanos recibidos y no interpreta nada. Los
// `static_assert` prueban que ESTE compilador produjo el layout que la
// especificacion describe; no dicen nada sobre lo que llegue por el socket.
// Comprobar que un datagrama mide lo que debe es del decoder, en ejecucion y
// antes de mirar el contenido (06-REALTIME-PIPELINE 3).
//
// ── ESTADO: BLOQUE B1 ───────────────────────────────────────────────────────
//
// Paquetes 0 (Motion), 1 (Session), 2 (Lap Data) y 3 (Event). Los 4-16 llegaran
// en los sub-bloques siguientes.
// ─────────────────────────────────────────────────────────────────────────────
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "pw/proto/f1_2026/constants.hpp"
#include "pw/proto/header.hpp"

namespace pw::proto::f1_2026 {

#pragma pack(push, 1)

// ── Paquete 0 — Motion ──────────────────────────────────────────────────────

/// Fisica y posicion de un coche. **54 B en 2026**, frente a 60 en 2025.
///
/// La diferencia no es un recorte: las tres g-forces pasaron de `float` a
/// `int16` cuantizado, y esos 6 B ahorrados por coche compensan de sobra los
/// dos coches anadidos. Es el unico paquete que ENCOGE entre formatos.
///
/// Un decoder que reutilizase la lectura de 2025 leeria basura desde
/// `m_gForceLateral` en adelante y sacaria un `yaw` absurdo. El `static_assert`
/// de tamano lo detecta al compilar.
struct CarMotionData {
    float         m_worldPositionX;      ///<  0: metros
    float         m_worldPositionY;      ///<  4
    float         m_worldPositionZ;      ///<  8
    float         m_worldVelocityX;      ///< 12: m/s
    float         m_worldVelocityY;      ///< 16
    float         m_worldVelocityZ;      ///< 20
    std::int16_t  m_worldForwardDirX;    ///< 24: normalizado, ÷ 32767,0
    std::int16_t  m_worldForwardDirY;    ///< 26
    std::int16_t  m_worldForwardDirZ;    ///< 28
    std::int16_t  m_worldRightDirX;      ///< 30: normalizado, ÷ 32767,0
    std::int16_t  m_worldRightDirY;      ///< 32
    std::int16_t  m_worldRightDirZ;      ///< 34
    std::int16_t  m_gForceLateral;       ///< 36: 2026 cuantizado, ÷ 1000,0
    std::int16_t  m_gForceLongitudinal;  ///< 38: idem
    std::int16_t  m_gForceVertical;      ///< 40: idem
    float         m_yaw;                 ///< 42: radianes
    float         m_pitch;               ///< 46
    float         m_roll;                ///< 50
};

/// Movimiento de los 24 coches. Sin campos despues del array.
struct PacketMotionData {
    PacketHeader                         m_header;
    std::array<CarMotionData, kMaxCars>  m_carMotionData;
};

// ── Paquete 1 — Session ─────────────────────────────────────────────────────

/// Zona de marshal: donde empieza y que bandera muestra. **5 B.**
struct MarshalZone {
    float        m_zoneStart;   ///< 0: fraccion 0..1 de la vuelta
    std::int8_t  m_zoneFlag;    ///< 4: -1 desconocido, 0 ninguna, 1 verde, 2 azul, 3 amarilla
};

/// Una muestra de la prevision meteorologica. **8 B.**
struct WeatherForecastSample {
    std::uint8_t m_sessionType;             ///< 0
    std::uint8_t m_timeOffset;              ///< 1: minutos
    std::uint8_t m_weather;                 ///< 2: 0 despejado … 5 tormenta
    std::int8_t  m_trackTemperature;        ///< 3: °C
    std::int8_t  m_trackTemperatureChange;  ///< 4: 0 sube, 1 baja, 2 sin cambio
    std::int8_t  m_airTemperature;          ///< 5: °C
    std::int8_t  m_airTemperatureChange;    ///< 6
    std::uint8_t m_rainPercentage;          ///< 7: 0-100
};

/// Zona de aerodinamica activa. **Sólo 2026.** 8 B.
struct ActiveAeroZone {
    float m_zoneStart;  ///< 0: fraccion 0..1 de la vuelta
    float m_zoneEnd;    ///< 4
};

/// Zona de DRS. **Sólo 2026**: en 2025 habia que inferirlas. 8 B.
struct DRSZone {
    float m_zoneStart;  ///< 0
    float m_zoneEnd;    ///< 4
};

/// Contexto de la sesion: 91 campos, **926 B**.
///
/// Es el paquete con mas campos del formato y el que mas creció en 2026 —de 753
/// a 926 B—, porque incorpora las zonas de Active Aero y de DRS, que antes
/// habia que adivinar. El orden es el oficial y no es agrupable por temas: los
/// cuatro campos de unidades van entre `m_sessionLength` y los contadores de
/// Safety Car, las veinticuatro reglas de simulacion van seguidas, y los trece
/// campos nuevos de 2026 NO estan todos juntos —`m_sector2/3LapDistanceStart`
/// quedan antes del bloque de aero—.
///
/// Los contadores `m_numXxx` que preceden a cada array vienen de la red y **no
/// estan acotados aqui**: acotarlos antes de usarlos es SEC-06, en F1.2a-11.
struct PacketSessionData {
    PacketHeader  m_header;                          ///<   0
    std::uint8_t  m_weather;                         ///<  29: 0 despejado … 5 tormenta
    std::int8_t   m_trackTemperature;                ///<  30: °C
    std::int8_t   m_airTemperature;                  ///<  31: °C
    std::uint8_t  m_totalLaps;                       ///<  32
    std::uint16_t m_trackLength;                     ///<  33: metros
    std::uint8_t  m_sessionType;                     ///<  35
    std::int8_t   m_trackId;                         ///<  36: -1 desconocido
    std::uint8_t  m_formula;                         ///<  37: 2026 anade 13 = F1 26
    std::uint16_t m_sessionTimeLeft;                 ///<  38: segundos
    std::uint16_t m_sessionDuration;                 ///<  40: segundos
    std::uint8_t  m_pitSpeedLimit;                   ///<  42: km/h
    std::uint8_t  m_gamePaused;                      ///<  43: solo partida en red
    std::uint8_t  m_isSpectating;                    ///<  44
    std::uint8_t  m_spectatorCarIndex;               ///<  45
    std::uint8_t  m_sliProNativeSupport;             ///<  46
    std::uint8_t  m_numMarshalZones;                 ///<  47: sin acotar (SEC-06)
    std::array<MarshalZone, kMaxMarshalZonesPerLap> m_marshalZones;   ///<  48
    std::uint8_t  m_safetyCarStatus;                 ///< 153: 0 ninguno … 3 vuelta de formacion
    std::uint8_t  m_networkGame;                     ///< 154: 0 offline, 1 online
    std::uint8_t  m_numWeatherForecastSamples;       ///< 155: sin acotar (SEC-06)
    std::array<WeatherForecastSample, kMaxWeatherForecastSamples> m_weatherForecastSamples;  ///< 156
    std::uint8_t  m_forecastAccuracy;                ///< 668: 0 perfecta, 1 aproximada
    std::uint8_t  m_aiDifficulty;                    ///< 669: 0-110
    std::uint32_t m_seasonLinkIdentifier;            ///< 670
    std::uint32_t m_weekendLinkIdentifier;           ///< 674
    std::uint32_t m_sessionLinkIdentifier;           ///< 678
    std::uint8_t  m_pitStopWindowIdealLap;           ///< 682: sólo jugador
    std::uint8_t  m_pitStopWindowLatestLap;          ///< 683: sólo jugador
    std::uint8_t  m_pitStopRejoinPosition;           ///< 684: sólo jugador
    std::uint8_t  m_steeringAssist;                  ///< 685: las nueve asistencias
    std::uint8_t  m_brakingAssist;                   ///< 686
    std::uint8_t  m_gearboxAssist;                   ///< 687: 1 manual … 3 automatica
    std::uint8_t  m_pitAssist;                       ///< 688
    std::uint8_t  m_pitReleaseAssist;                ///< 689
    std::uint8_t  m_ERSAssist;                       ///< 690
    std::uint8_t  m_DRSAssist;                       ///< 691
    std::uint8_t  m_dynamicRacingLine;               ///< 692: 0 off, 1 solo curvas, 2 completa
    std::uint8_t  m_dynamicRacingLineType;           ///< 693: 0 = 2D, 1 = 3D
    std::uint8_t  m_gameMode;                        ///< 694
    std::uint8_t  m_ruleSet;                         ///< 695
    std::uint32_t m_timeOfDay;                       ///< 696: minutos desde medianoche
    std::uint8_t  m_sessionLength;                   ///< 700: 0 ninguna, 2 muy corta … 7 completa
    std::uint8_t  m_speedUnitsLeadPlayer;            ///< 701: las cuatro unidades
    std::uint8_t  m_temperatureUnitsLeadPlayer;      ///< 702
    std::uint8_t  m_speedUnitsSecondaryPlayer;       ///< 703
    std::uint8_t  m_temperatureUnitsSecondaryPlayer; ///< 704
    std::uint8_t  m_numSafetyCarPeriods;             ///< 705
    std::uint8_t  m_numVirtualSafetyCarPeriods;      ///< 706
    std::uint8_t  m_numRedFlagPeriods;               ///< 707
    std::uint8_t  m_equalCarPerformance;             ///< 708: las 24 reglas de simulacion
    std::uint8_t  m_recoveryMode;                    ///< 709
    std::uint8_t  m_flashbackLimit;                  ///< 710
    std::uint8_t  m_surfaceType;                     ///< 711
    std::uint8_t  m_lowFuelMode;                     ///< 712
    std::uint8_t  m_raceStarts;                      ///< 713: 0 manual, 1 asistida
    std::uint8_t  m_tyreTemperature;                 ///< 714
    std::uint8_t  m_pitLaneTyreSim;                  ///< 715
    std::uint8_t  m_carDamage;                       ///< 716
    std::uint8_t  m_carDamageRate;                   ///< 717
    std::uint8_t  m_collisions;                      ///< 718
    std::uint8_t  m_collisionsOffForFirstLapOnly;    ///< 719
    std::uint8_t  m_mpUnsafePitRelease;              ///< 720
    std::uint8_t  m_mpOffForGriefing;                ///< 721
    std::uint8_t  m_cornerCuttingStringency;         ///< 722
    std::uint8_t  m_parcFermeRules;                  ///< 723
    std::uint8_t  m_pitStopExperience;               ///< 724
    std::uint8_t  m_safetyCar;                       ///< 725
    std::uint8_t  m_safetyCarExperience;             ///< 726
    std::uint8_t  m_formationLap;                    ///< 727
    std::uint8_t  m_formationLapExperience;          ///< 728
    std::uint8_t  m_redFlags;                        ///< 729
    std::uint8_t  m_affectsLicenceLevelSolo;         ///< 730
    std::uint8_t  m_affectsLicenceLevelMP;           ///< 731
    std::uint8_t  m_numSessionsInWeekend;            ///< 732: sin acotar (SEC-06)
    std::array<std::uint8_t, kMaxSessionsInWeekend> m_weekendStructure;  ///< 733
    float         m_sector2LapDistanceStart;         ///< 745: metros. Evita inferir sectores
    float         m_sector3LapDistanceStart;         ///< 749: metros
    // Zonas de aero y DRS: todo lo que sigue es exclusivo de 2026.
    std::uint8_t  m_activeAeroTrackStatus;           ///< 753: 0 completo, 1 parcial
    std::uint8_t  m_numActiveAeroZonesFull;          ///< 754: sin acotar (SEC-06)
    std::array<ActiveAeroZone, kMaxActiveAeroZonesPerLap> m_activeAeroZonesFull;     ///< 755
    std::uint8_t  m_numActiveAeroZonesPartial;       ///< 819: sin acotar (SEC-06)
    std::array<ActiveAeroZone, kMaxActiveAeroZonesPerLap> m_activeAeroZonesPartial;  ///< 820
    std::uint8_t  m_numDRSZones;                     ///< 884: sin acotar (SEC-06)
    std::array<DRSZone, kMaxDrsZones> m_drsZones;    ///< 885
    float         m_startReactionTime;               ///< 917: s. 0.0f si las salidas son asistidas
    std::uint8_t  m_antiLockBrakesAssist;            ///< 921
    std::uint8_t  m_tractionControlAssist;           ///< 922: 0 off, 1 medio, 2 completo
    std::uint8_t  m_dynamicRacingLineHiVis;          ///< 923
    std::uint8_t  m_dynamicRacingLineColourBlind;    ///< 924: 0 off … 3 tritanopia
    std::uint8_t  m_recurringRewindPrompt;           ///< 925
};

// ── Paquete 2 — Lap Data ────────────────────────────────────────────────────

/// Cronometraje de un coche. **57 B en ambos formatos.**
///
/// Sectores y deltas viajan en DOS PARTES -minutos enteros y milisegundos- y
/// hay que recomponerlos: `minutos * 60000 + ms`. Olvidarlo da tiempos
/// correctos por debajo del minuto e incorrectos por encima, que es justo el
/// caso de un sector largo o de un delta grande bajo Safety Car
/// (03-UDP-PROTOCOL 6.2).
///
/// Los centinelas -255 en `m_speedTrapFastestLap`- se transcriben TAL CUAL.
/// Convertirlos en ausencia es de la normalizacion (RF-109, F1.3-09).
struct LapData {
    std::uint32_t m_lastLapTimeInMS;              ///<  0
    std::uint32_t m_currentLapTimeInMS;           ///<  4
    std::uint16_t m_sector1TimeMSPart;            ///<  8
    std::uint8_t  m_sector1TimeMinutesPart;       ///< 10
    std::uint16_t m_sector2TimeMSPart;            ///< 11
    std::uint8_t  m_sector2TimeMinutesPart;       ///< 13
    std::uint16_t m_deltaToCarInFrontMSPart;      ///< 14
    std::uint8_t  m_deltaToCarInFrontMinutesPart; ///< 16
    std::uint16_t m_deltaToRaceLeaderMSPart;      ///< 17
    std::uint8_t  m_deltaToRaceLeaderMinutesPart; ///< 19
    float         m_lapDistance;                  ///< 20: m. PUEDE SER NEGATIVO
    float         m_totalDistance;                ///< 24: m. Puede ser negativo
    float         m_safetyCarDelta;               ///< 28: s
    std::uint8_t  m_carPosition;                  ///< 32
    std::uint8_t  m_currentLapNum;                ///< 33
    std::uint8_t  m_pitStatus;                    ///< 34: 0 ninguno, 1 entrando, 2 en boxes
    std::uint8_t  m_numPitStops;                  ///< 35
    std::uint8_t  m_sector;                       ///< 36: 0 S1, 1 S2, 2 S3
    std::uint8_t  m_currentLapInvalid;            ///< 37: 0 valida, 1 invalida
    std::uint8_t  m_penalties;                    ///< 38: s acumulados
    std::uint8_t  m_totalWarnings;                ///< 39
    std::uint8_t  m_cornerCuttingWarnings;        ///< 40
    std::uint8_t  m_numUnservedDriveThroughPens;  ///< 41
    std::uint8_t  m_numUnservedStopGoPens;        ///< 42
    std::uint8_t  m_gridPosition;                 ///< 43
    std::uint8_t  m_driverStatus;                 ///< 44: 0 garaje … 4 en pista
    std::uint8_t  m_resultStatus;                 ///< 45: 0 invalido … 7 retirado
    std::uint8_t  m_pitLaneTimerActive;           ///< 46
    std::uint16_t m_pitLaneTimeInLaneInMS;        ///< 47
    std::uint16_t m_pitStopTimerInMS;             ///< 49
    std::uint8_t  m_pitStopShouldServePen;        ///< 51
    float         m_speedTrapFastestSpeed;        ///< 52: km/h
    std::uint8_t  m_speedTrapFastestLap;          ///< 56: 255 = sin registrar
};

/// Cronometraje de los 24 coches, con dos indices de Time Trial al final.
struct PacketLapData {
    PacketHeader                  m_header;
    std::array<LapData, kMaxCars> m_lapData;
    std::uint8_t                  m_timeTrialPBCarIdx;     ///< 255 = invalido
    std::uint8_t                  m_timeTrialRivalCarIdx;  ///< 255 = invalido
};

// ── Paquete 3 — Event ───────────────────────────────────────────────────────

/// Bytes de la union `EventDataDetails`: **12 B fijos**.
///
/// Aqui se representa como almacenamiento sin interpretar, y es deliberado. El
/// contenido SOLO puede leerse segun el codigo de evento recibido: leer el
/// miembro equivocado produce valores plausibles y falsos, que es peor que un
/// error. Esa interpretacion -los 25 codigos, incluidos los cuatro de 2026- es
/// F1.2a-10 y todavia no existe.
///
/// Representar los bytes y entender los bytes son dos trabajos distintos; este
/// fichero solo hace el primero.
inline constexpr std::size_t kEventDetailsBytes = 12;

/// Longitud del codigo de evento: `cs_eventStringCodeLen` en la especificacion.
inline constexpr std::size_t kEventStringCodeLength = 4;

/// Suceso discreto. **45 B fijos**: 29 de cabecera + 4 de codigo + 12 de union.
///
/// El codigo es `uint8[4]` en la especificacion, no una cadena: **no lleva
/// terminador nulo**, y tratarlo como `char*` leeria dentro de la union.
struct PacketEventData {
    PacketHeader                                       m_header;           ///<  0
    std::array<std::uint8_t, kEventStringCodeLength>    m_eventStringCode;  ///< 29: «SSTA», «FLBK»…
    std::array<std::byte, kEventDetailsBytes>           m_eventDetails;     ///< 33: union sin interpretar
};

#pragma pack(pop)

// ─────────────────────────────────────────────────────────────────────────────
// Los cuatro niveles de verificacion de `10-TESTING-PLAN.md` §3.
//
// Cada nivel caza un error que los otros no ven: el tamano total no distingue
// dos campos intercambiados del mismo ancho; la aritmetica delata un campo de
// mas o de menos que compense; los offsets encuentran el campo movido; y el
// layout descubre un `#pragma pack` mal cerrado.
// ─────────────────────────────────────────────────────────────────────────────

// ── Nivel 1: tamano total, contra la UNICA tabla del formato ────────────────
//
// Se comparan contra `expectedSize`, no contra un numero escrito aqui: si un
// dia la tabla y los structs discrepasen, no compilaria ninguno de los dos.
static_assert(sizeof(PacketMotionData)  == expectedSize(PacketId::Motion).value());
static_assert(sizeof(PacketSessionData) == expectedSize(PacketId::Session).value());
static_assert(sizeof(PacketLapData)     == expectedSize(PacketId::LapData).value());
static_assert(sizeof(PacketEventData)   == expectedSize(PacketId::Event).value());

static_assert(sizeof(CarMotionData) == 54, "54 B por coche en 2026 (60 en 2025)");
static_assert(sizeof(LapData)       == 57, "57 B por coche en ambos formatos");

static_assert(sizeof(MarshalZone)           ==  5, "float + int8, sin relleno");
static_assert(sizeof(WeatherForecastSample) ==  8, "ocho campos de un byte");
static_assert(sizeof(ActiveAeroZone)        ==  8, "dos float");
static_assert(sizeof(DRSZone)               ==  8, "dos float");

// ── Nivel 2: coherencia aritmetica ──────────────────────────────────────────
static_assert(sizeof(PacketMotionData) == sizeof(PacketHeader) + kMaxCars * sizeof(CarMotionData),
              "cabecera + 24 coches, sin nada detras del array");
static_assert(sizeof(PacketLapData)
                  == sizeof(PacketHeader) + kMaxCars * sizeof(LapData) + 2 * sizeof(std::uint8_t),
              "cabecera + 24 coches + los dos indices de Time Trial");
static_assert(sizeof(PacketEventData)
                  == sizeof(PacketHeader) + kEventStringCodeLength + kEventDetailsBytes,
              "cabecera + codigo de 4 bytes + union de 12 B");

// Session, desglosado por partes: es la unica forma de que un campo perdido se
// note, porque con 91 campos el tamano total por si solo no dice donde fallo.
static_assert(sizeof(PacketSessionData)
                  == sizeof(PacketHeader)
                         + 18                                              // escalares previos
                         + 1 + kMaxMarshalZonesPerLap * sizeof(MarshalZone)
                         + 3
                         + kMaxWeatherForecastSamples * sizeof(WeatherForecastSample)
                         + 2 + 12 + 3                                      // precision, IA, enlaces, pit
                         + 9                                               // asistencias
                         + 2 + 4 + 1                                       // modo, reglas, hora, duracion
                         + 4                                               // unidades
                         + 3                                               // contadores de sesion
                         + 24                                              // reglas de simulacion
                         + 1 + kMaxSessionsInWeekend                       // estructura del fin de semana
                         + 8                                               // sectores 2 y 3
                         + 2 + kMaxActiveAeroZonesPerLap * sizeof(ActiveAeroZone)
                         + 1 + kMaxActiveAeroZonesPerLap * sizeof(ActiveAeroZone)
                         + 1 + kMaxDrsZones * sizeof(DRSZone)
                         + 4                                               // startReactionTime
                         + 5,                                              // asistencias de 2026
              "los 91 campos de Session suman 897 B de cuerpo");

static_assert(sizeof(PacketSessionData) - sizeof(PacketHeader) == 897,
              "cuerpo de Session: 897 B");

// La suma de los campos de CarMotionData, uno a uno: si 2026 volviera a
// cuantizar o a ampliar un campo, esto lo senala antes que el tamano total.
static_assert(sizeof(CarMotionData) == 6 * sizeof(float)      // posicion y velocidad
                                           + 6 * sizeof(std::int16_t)   // direcciones normalizadas
                                           + 3 * sizeof(std::int16_t)   // g-forces cuantizadas
                                           + 3 * sizeof(float),         // yaw, pitch, roll
              "las g-forces de 2026 son int16, no float");

// ── Nivel 3: offsets criticos ───────────────────────────────────────────────
//
// No se comprueban todos los campos: se eligen los que detectan errores reales.
// El comienzo de cada array, el campo que va DESPUES de un array, los campos
// que cambiaron en 2026 y los que quedarian mal alineados si el empaquetado
// fallara.

// Motion: el array empieza justo tras la cabecera.
static_assert(offsetof(PacketMotionData, m_header)        ==  0);
static_assert(offsetof(PacketMotionData, m_carMotionData) == 29);

// Dentro de un coche: la primera g-force cuantizada y lo que viene despues. Si
// alguien reintrodujera los `float` de 2025, estos dos offsets se moverian 6 B.
static_assert(offsetof(CarMotionData, m_worldForwardDirX) == 24);
static_assert(offsetof(CarMotionData, m_gForceLateral)    == 36);
static_assert(offsetof(CarMotionData, m_yaw)              == 42);

// Lap Data: los dos indices van DESPUES del array, que es donde un error de
// cardinalidad se nota.
static_assert(offsetof(PacketLapData, m_lapData)              ==   29);
static_assert(offsetof(PacketLapData, m_timeTrialPBCarIdx)    == 1397);
static_assert(offsetof(PacketLapData, m_timeTrialRivalCarIdx) == 1398);

// Dentro de una vuelta: el primer `float` tras la tanda de tiempos en dos
// partes -desalineado a proposito, en el offset 20- y el ultimo campo.
static_assert(offsetof(LapData, m_lapDistance)           == 20);
static_assert(offsetof(LapData, m_safetyCarDelta)        == 28);
static_assert(offsetof(LapData, m_pitLaneTimeInLaneInMS) == 47);
static_assert(offsetof(LapData, m_speedTrapFastestSpeed) == 52);
static_assert(offsetof(LapData, m_speedTrapFastestLap)   == 56);

// Event: el codigo y el comienzo de la union.
static_assert(offsetof(PacketEventData, m_eventStringCode) == 29);
static_assert(offsetof(PacketEventData, m_eventDetails)    == 33);

// Session: cada array, el campo que va justo DESPUES de cada array -donde se
// nota una cardinalidad equivocada- y los campos de 2026. Todos derivados del
// orden oficial, no copiados de ningun ejemplo.
static_assert(offsetof(PacketSessionData, m_numMarshalZones)           ==  47);
static_assert(offsetof(PacketSessionData, m_marshalZones)              ==  48);
static_assert(offsetof(PacketSessionData, m_safetyCarStatus)           == 153);  // tras 21 zonas
static_assert(offsetof(PacketSessionData, m_numWeatherForecastSamples) == 155);
static_assert(offsetof(PacketSessionData, m_weatherForecastSamples)    == 156);
static_assert(offsetof(PacketSessionData, m_forecastAccuracy)          == 668);  // tras 64 muestras
static_assert(offsetof(PacketSessionData, m_seasonLinkIdentifier)      == 670);  // uint32 desalineado
static_assert(offsetof(PacketSessionData, m_steeringAssist)            == 685);
static_assert(offsetof(PacketSessionData, m_timeOfDay)                 == 696);
static_assert(offsetof(PacketSessionData, m_speedUnitsLeadPlayer)      == 701);
static_assert(offsetof(PacketSessionData, m_equalCarPerformance)       == 708);
static_assert(offsetof(PacketSessionData, m_affectsLicenceLevelMP)     == 731);  // ultima regla
static_assert(offsetof(PacketSessionData, m_numSessionsInWeekend)      == 732);
static_assert(offsetof(PacketSessionData, m_weekendStructure)          == 733);
static_assert(offsetof(PacketSessionData, m_sector2LapDistanceStart)   == 745);
static_assert(offsetof(PacketSessionData, m_sector3LapDistanceStart)   == 749);
static_assert(offsetof(PacketSessionData, m_activeAeroTrackStatus)     == 753);
static_assert(offsetof(PacketSessionData, m_activeAeroZonesFull)       == 755);
static_assert(offsetof(PacketSessionData, m_numActiveAeroZonesPartial) == 819);
static_assert(offsetof(PacketSessionData, m_activeAeroZonesPartial)    == 820);
static_assert(offsetof(PacketSessionData, m_numDRSZones)               == 884);
static_assert(offsetof(PacketSessionData, m_drsZones)                  == 885);
static_assert(offsetof(PacketSessionData, m_startReactionTime)         == 917);
static_assert(offsetof(PacketSessionData, m_recurringRewindPrompt)     == 925);  // ultimo campo

static_assert(offsetof(MarshalZone, m_zoneFlag)                  == 4);
static_assert(offsetof(WeatherForecastSample, m_rainPercentage)  == 7);
static_assert(offsetof(ActiveAeroZone, m_zoneEnd)                == 4);
static_assert(offsetof(DRSZone, m_zoneEnd)                       == 4);

// ── Nivel 4: requisitos de layout ───────────────────────────────────────────
static_assert(std::is_trivially_copyable_v<CarMotionData>);
static_assert(std::is_trivially_copyable_v<PacketMotionData>);
static_assert(std::is_trivially_copyable_v<MarshalZone>);
static_assert(std::is_trivially_copyable_v<WeatherForecastSample>);
static_assert(std::is_trivially_copyable_v<ActiveAeroZone>);
static_assert(std::is_trivially_copyable_v<DRSZone>);
static_assert(std::is_trivially_copyable_v<PacketSessionData>);
static_assert(std::is_trivially_copyable_v<LapData>);
static_assert(std::is_trivially_copyable_v<PacketLapData>);
static_assert(std::is_trivially_copyable_v<PacketEventData>);

static_assert(std::is_standard_layout_v<CarMotionData>);
static_assert(std::is_standard_layout_v<PacketMotionData>);
static_assert(std::is_standard_layout_v<MarshalZone>);
static_assert(std::is_standard_layout_v<WeatherForecastSample>);
static_assert(std::is_standard_layout_v<ActiveAeroZone>);
static_assert(std::is_standard_layout_v<DRSZone>);
static_assert(std::is_standard_layout_v<PacketSessionData>);
static_assert(std::is_standard_layout_v<LapData>);
static_assert(std::is_standard_layout_v<PacketLapData>);
static_assert(std::is_standard_layout_v<PacketEventData>);

static_assert(alignof(CarMotionData)         == 1);
static_assert(alignof(PacketMotionData)      == 1);
static_assert(alignof(MarshalZone)           == 1);
static_assert(alignof(WeatherForecastSample) == 1);
static_assert(alignof(ActiveAeroZone)        == 1);
static_assert(alignof(DRSZone)               == 1);
static_assert(alignof(PacketSessionData)     == 1);
static_assert(alignof(LapData)               == 1);
static_assert(alignof(PacketLapData)         == 1);
static_assert(alignof(PacketEventData)       == 1);

} // namespace pw::proto::f1_2026
