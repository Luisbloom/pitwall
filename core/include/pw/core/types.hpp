#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Tipos fuertes del nucleo (F0-05).
//
// Un indice de vehiculo y un numero de vuelta son los dos enteros pequenos, y
// confundirlos es un error que el compilador puede impedir. StrongId encapsula
// un entero sin permitir conversiones implicitas ni mezclas entre etiquetas
// distintas (09-BACKEND-PLAN 1.1: «enum class siempre», misma motivacion).
// ─────────────────────────────────────────────────────────────────────────────
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace pw {

/// Entero con identidad de tipo. `Tag` solo sirve para separar instancias.
template <typename T, typename Tag>
class StrongId {
    static_assert(std::is_integral_v<T>, "StrongId solo envuelve tipos enteros");

public:
    using value_type = T;

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(T v) noexcept : value_{v} {}

    [[nodiscard]] constexpr T get() const noexcept { return value_; }

    constexpr auto operator<=>(const StrongId&) const noexcept = default;
    constexpr bool operator==(const StrongId&) const noexcept = default;

private:
    T value_{};
};

/// Indice de coche dentro del paquete, 0..kMaxCars-1. Nunca es una posicion.
using VehicleIndex = StrongId<std::uint8_t, struct VehicleIndexTag>;

/// Numero de vuelta tal y como lo cuenta el juego, empezando en 1.
using LapNumber = StrongId<std::uint8_t, struct LapNumberTag>;

/// Identidad de una sesion: el m_sessionUID de la cabecera.
using SessionKey = StrongId<std::uint64_t, struct SessionKeyTag>;

/// Rama de la linea temporal de una sesion. 0 es la rama original; cada
/// flashback abre una nueva (RF-207).
using BranchId = StrongId<std::uint32_t, struct BranchIdTag>;

} // namespace pw

namespace std {

template <typename T, typename Tag>
struct hash<pw::StrongId<T, Tag>> {
    [[nodiscard]] size_t operator()(const pw::StrongId<T, Tag>& id) const noexcept
    {
        return hash<T>{}(id.get());
    }
};

} // namespace std
