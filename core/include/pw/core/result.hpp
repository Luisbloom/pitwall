#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::Result<T, E> — resultado o error, sin excepciones (F0-05).
//
// Un datagrama malformado no es excepcional: es lo normal (09-BACKEND-PLAN 5).
// Los errores esperados se modelan como VALOR DE RETORNO; las excepciones se
// reservan para fallos de arranque.
//
// C++23 aporta std::expected. Donde el compilador lo ofrece, Result es un alias
// de std::expected (ADR-001: «C++20 con adopcion selectiva de C++23»). Donde no,
// esta cabecera proporciona una implementacion con el mismo subconjunto de API,
// de modo que el codigo de PitWall es identico en ambos casos.
// ─────────────────────────────────────────────────────────────────────────────
#include <exception>
#include <type_traits>
#include <utility>
#include <variant>
#include <version>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#  define PW_HAS_STD_EXPECTED 1
#  include <expected>
#else
#  define PW_HAS_STD_EXPECTED 0
#  include <optional>
#endif

namespace pw {

#if PW_HAS_STD_EXPECTED

template <typename T, typename E>
using Result = std::expected<T, E>;

template <typename E>
using Unexpected = std::unexpected<E>;

#else

/// Se lanza al pedir el valor de un Result que contiene un error.
class BadResultAccess : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override
    {
        return "pw::Result: se ha pedido el valor de un resultado con error";
    }
};

/// Envoltorio que marca «esto es un error», no un valor.
template <typename E>
class Unexpected {
public:
    constexpr explicit Unexpected(const E& e) : error_{e} {}
    constexpr explicit Unexpected(E&& e) noexcept : error_{std::move(e)} {}

    [[nodiscard]] constexpr const E&  error() const&  noexcept { return error_; }
    [[nodiscard]] constexpr E&&       error() &&      noexcept { return std::move(error_); }

private:
    E error_;
};

template <typename E>
Unexpected(E) -> Unexpected<E>;

/// Contiene un valor de tipo T o un error de tipo E. Nunca los dos, nunca ninguno.
template <typename T, typename E>
class Result {
    static_assert(!std::is_same_v<T, E>, "Result<T, E> exige que T y E sean tipos distintos");

public:
    using value_type = T;
    using error_type = E;

    constexpr Result() : storage_{std::in_place_index<0>, T{}} {}
    constexpr Result(const T& v) : storage_{std::in_place_index<0>, v} {}          // NOLINT
    constexpr Result(T&& v) : storage_{std::in_place_index<0>, std::move(v)} {}    // NOLINT
    constexpr Result(const Unexpected<E>& u) : storage_{std::in_place_index<1>, u.error()} {}  // NOLINT
    constexpr Result(Unexpected<E>&& u) : storage_{std::in_place_index<1>, std::move(u).error()} {}  // NOLINT

    [[nodiscard]] constexpr bool has_value() const noexcept { return storage_.index() == 0; }
    constexpr explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] constexpr const T& value() const&
    {
        if (!has_value()) { throw BadResultAccess{}; }
        return std::get<0>(storage_);
    }
    [[nodiscard]] constexpr T& value() &
    {
        if (!has_value()) { throw BadResultAccess{}; }
        return std::get<0>(storage_);
    }

    [[nodiscard]] constexpr const E& error() const& noexcept { return std::get<1>(storage_); }

    template <typename U>
    [[nodiscard]] constexpr T value_or(U&& fallback) const&
    {
        return has_value() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
    }

    [[nodiscard]] constexpr const T& operator*() const& noexcept { return std::get<0>(storage_); }
    [[nodiscard]] constexpr const T* operator->() const noexcept { return &std::get<0>(storage_); }

private:
    std::variant<T, E> storage_;
};

/// Especializacion para operaciones que no devuelven nada pero pueden fallar.
template <typename E>
class Result<void, E> {
public:
    using value_type = void;
    using error_type = E;

    constexpr Result() noexcept = default;
    constexpr Result(const Unexpected<E>& u) : error_{u.error()} {}                      // NOLINT
    constexpr Result(Unexpected<E>&& u) : error_{std::move(u).error()} {}                // NOLINT

    [[nodiscard]] constexpr bool has_value() const noexcept { return !error_.has_value(); }
    constexpr explicit operator bool() const noexcept { return has_value(); }

    constexpr void value() const
    {
        if (!has_value()) { throw BadResultAccess{}; }
    }

    [[nodiscard]] constexpr const E& error() const& noexcept { return *error_; }

private:
    std::optional<E> error_{};
};

#endif // PW_HAS_STD_EXPECTED

/// Construye el lado de error de un Result sin nombrar su tipo de valor.
template <typename E>
[[nodiscard]] constexpr auto makeError(E&& e)
{
    return Unexpected<std::decay_t<E>>{std::forward<E>(e)};
}

} // namespace pw
