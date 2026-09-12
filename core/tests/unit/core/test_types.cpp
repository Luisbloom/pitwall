// Pruebas de los tipos fuertes: lo que el compilador ya no deja confundir.
#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <unordered_map>

#include "pw/core/types.hpp"

using pw::BranchId;
using pw::LapNumber;
using pw::SessionKey;
using pw::VehicleIndex;

TEST_CASE("Los tipos fuertes no se construyen implicitamente desde su entero", "[core][types]")
{
    STATIC_REQUIRE_FALSE(std::is_convertible_v<std::uint8_t, VehicleIndex>);
    STATIC_REQUIRE_FALSE(std::is_convertible_v<VehicleIndex, std::uint8_t>);
    STATIC_REQUIRE(std::is_constructible_v<VehicleIndex, std::uint8_t>);
}

TEST_CASE("Dos tipos fuertes distintos no son el mismo tipo", "[core][types]")
{
    STATIC_REQUIRE_FALSE(std::is_same_v<VehicleIndex, LapNumber>);
    STATIC_REQUIRE_FALSE(std::is_convertible_v<VehicleIndex, LapNumber>);
    STATIC_REQUIRE_FALSE(std::is_same_v<SessionKey, BranchId>);
}

TEST_CASE("Los tipos fuertes comparan y ordenan por su valor", "[core][types]")
{
    REQUIRE(VehicleIndex{3} == VehicleIndex{3});
    REQUIRE(VehicleIndex{3} != VehicleIndex{4});
    REQUIRE(VehicleIndex{3} < VehicleIndex{4});
    REQUIRE(LapNumber{12}.get() == 12);
    REQUIRE(SessionKey{0xDEADBEEFCAFEULL}.get() == 0xDEADBEEFCAFEULL);
}

TEST_CASE("Un tipo fuerte por defecto vale cero", "[core][types]")
{
    STATIC_REQUIRE(VehicleIndex{}.get() == 0);
    STATIC_REQUIRE(BranchId{}.get() == 0);   // rama 0 = linea temporal original
}

TEST_CASE("Los tipos fuertes sirven de clave en tablas asociativas", "[core][types]")
{
    std::unordered_map<VehicleIndex, int> positions;
    positions[VehicleIndex{7}]  = 1;
    positions[VehicleIndex{11}] = 2;

    REQUIRE(positions.at(VehicleIndex{7}) == 1);
    REQUIRE(positions.count(VehicleIndex{99}) == 0);
}
