// Pruebas de Result<T, E> — errores como valor, no como excepcion.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "pw/core/result.hpp"

namespace {

enum class BindError { PortBusy, PermissionDenied };

pw::Result<int, BindError> bind(const int port)
{
    if (port == 20777) { return 4; }
    return pw::makeError(BindError::PortBusy);
}

pw::Result<void, std::string> writeCapture(const bool diskFull)
{
    if (diskFull) { return pw::makeError(std::string{"disco lleno"}); }
    return {};
}

} // namespace

TEST_CASE("Un Result correcto lleva su valor", "[core][result]")
{
    const auto socket = bind(20777);

    REQUIRE(socket.has_value());
    REQUIRE(static_cast<bool>(socket));
    REQUIRE(socket.value() == 4);
}

TEST_CASE("Un Result con error no se confunde con un valor", "[core][result]")
{
    const auto socket = bind(80);

    REQUIRE_FALSE(socket.has_value());
    REQUIRE_FALSE(static_cast<bool>(socket));
    REQUIRE(socket.error() == BindError::PortBusy);
}

TEST_CASE("value_or evita tener que comprobar", "[core][result]")
{
    REQUIRE(bind(20777).value_or(-1) == 4);
    REQUIRE(bind(80).value_or(-1) == -1);
}

TEST_CASE("Result<void, E> distingue exito de fallo sin valor", "[core][result]")
{
    const auto ok = writeCapture(false);
    REQUIRE(ok.has_value());

    const auto failed = writeCapture(true);
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error() == "disco lleno");
}
