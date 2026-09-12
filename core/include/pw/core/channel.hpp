#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::Channel<T> — valor y procedencia, inseparables (F0-06, ADR-009).
//
// El error mas frecuente del ecosistema es tratar el cero de una telemetria
// restringida como un cero real, y pintar «0 % de desgaste» de forma falsa
// (RN-001). Channel<T> convierte esa convencion en una restriccion del
// compilador: NO existe conversion implicita a T, asi que obtener el valor
// exige haber comprobado known().
//
// La prueba de NO COMPILACION que verifica esa propiedad vive en
// core/tests/unit/nocompile/ y la ejecuta CTest (F0-06).
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "pw/core/reason.hpp"

namespace pw {

/// Taxonomia obligatoria de procedencia (01-REQUIREMENTS.md 19.6).
enum class Provenance : std::uint8_t {
    Raw = 0,     ///< Tal como lo emite el juego, salvo desescalado documentado.
    Derived,     ///< Calculado de forma determinista a partir de valores Raw.
    Estimated,   ///< Resultado de un modelo con incertidumbre.
    Unavailable, ///< El juego no proporciona el dato en este contexto.
    Restricted   ///< Existe, pero su propietario no lo comparte.
};

/// Nombre estable en minusculas, para logs y para el transporte interno.
[[nodiscard]] const char* provenanceName(Provenance provenance) noexcept;

/// Se lanza al pedir value() de un canal sin valor conocido.
class ChannelAccessError : public std::logic_error {
public:
    ChannelAccessError() : std::logic_error{"pw::Channel: value() sin comprobar known()"} {}
};

template <typename T>
class Channel {
public:
    using value_type = T;

    /// Por defecto: todavia no se ha recibido nada.
    constexpr Channel() noexcept = default;

    [[nodiscard]] static constexpr Channel raw(T v) noexcept
    {
        return Channel{std::move(v), Provenance::Raw, 0.0F};
    }
    [[nodiscard]] static constexpr Channel derived(T v) noexcept
    {
        return Channel{std::move(v), Provenance::Derived, 0.0F};
    }
    [[nodiscard]] static constexpr Channel estimated(T v, float uncertainty = 0.0F) noexcept
    {
        return Channel{std::move(v), Provenance::Estimated, uncertainty};
    }
    [[nodiscard]] static constexpr Channel unavailable(Reason reason) noexcept
    {
        Channel c;
        c.provenance_ = Provenance::Unavailable;
        c.reason_     = reason;
        return c;
    }
    /// Especializacion de unavailable: el dato existe pero su dueno no lo comparte.
    [[nodiscard]] static constexpr Channel restricted() noexcept
    {
        Channel c;
        c.provenance_ = Provenance::Restricted;
        c.reason_     = Reason::RestrictedByOwner;
        return c;
    }

    [[nodiscard]] constexpr bool       known()       const noexcept { return value_.has_value(); }
    [[nodiscard]] constexpr Provenance provenance()  const noexcept { return provenance_; }
    [[nodiscard]] constexpr Reason     reason()      const noexcept { return reason_; }
    [[nodiscard]] constexpr float      uncertainty() const noexcept { return uncertainty_; }

    /// Precondicion: known(). Lanza ChannelAccessError si no se ha comprobado.
    [[nodiscard]] constexpr const T& value() const&
    {
        if (!value_.has_value()) { throw ChannelAccessError{}; }
        return *value_;
    }

    /// Unica forma de leer sin comprobar: obliga a decidir el valor de respaldo.
    [[nodiscard]] constexpr T valueOr(T fallback) const noexcept
    {
        return value_.has_value() ? *value_ : fallback;
    }

    constexpr bool operator==(const Channel&) const = default;

private:
    constexpr Channel(T v, Provenance p, float uncertainty) noexcept
        : value_{std::move(v)}, provenance_{p}, reason_{Reason::None}, uncertainty_{uncertainty}
    {
    }

    std::optional<T> value_{};
    Provenance       provenance_{Provenance::Unavailable};
    Reason           reason_{Reason::NotYetReceived};
    float            uncertainty_{0.0F};
};

// La garantia central del ADR-009, verificada en cada compilacion de quien
// incluya esta cabecera. La prueba de no compilacion cubre el caso simetrico:
// que un uso real de la conversion implicita rompa la construccion.
static_assert(!std::is_convertible_v<Channel<float>, float>,
              "Channel no debe convertirse implicitamente a su valor");
static_assert(!std::is_convertible_v<Channel<bool>, bool>,
              "Channel no debe convertirse implicitamente a su valor");

} // namespace pw
