#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::Reason — por que un dato no esta disponible (F0-05).
//
// Complementa a Provenance: 'UNAVAILABLE' y 'RESTRICTED' dicen QUE no hay dato;
// Reason dice POR QUE. La interfaz necesita la diferencia para poder explicar
// «Active Aero no existe en el formato 2025» en lugar de un generico «sin dato»
// (01-REQUIREMENTS.md 19.6, RF-612).
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>

namespace pw {

enum class Reason : std::uint8_t {
    None = 0,             ///< El dato esta presente: no hay razon de ausencia.
    NotYetReceived,       ///< Aun no ha llegado ningun paquete que lo contenga.
    NotSupportedByFormat, ///< El formato en uso no transporta este canal (2026 -> 2025).
    NotApplicable,        ///< No tiene sentido en este contexto (Motion Ex de un rival).
    RestrictedByOwner,    ///< Existe, pero su propietario no lo comparte (RN-001).
    SentinelValue,        ///< El juego emitio un centinela de ausencia (255, 65535, -1).
    DecodeError,          ///< El paquete llego, pero no pudo interpretarse.
    Unknown               ///< Ausencia sin causa identificada.
};

/// Nombre estable en minusculas, apto para logs y para el transporte interno.
[[nodiscard]] const char* reasonName(Reason reason) noexcept;

/// Explicacion breve en espanol, pensada para mostrarse al usuario.
[[nodiscard]] const char* reasonDescription(Reason reason) noexcept;

} // namespace pw
