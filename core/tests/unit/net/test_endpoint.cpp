// Pruebas de Ipv4Endpoint (F1.1-04).
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "pw/net/endpoint.hpp"

using pw::net::Ipv4Endpoint;

TEST_CASE("Se analizan las direcciones que aparecen en la configuracion",
          "[net][endpoint][RF-003]")
{
    Ipv4Endpoint endpoint;

    REQUIRE(Ipv4Endpoint::parse("127.0.0.1", 20777, endpoint));
    REQUIRE(endpoint.address() == std::array<std::uint8_t, 4>{127, 0, 0, 1});
    REQUIRE(endpoint.port() == 20777);
    REQUIRE(endpoint.isLoopback());
    REQUIRE_FALSE(endpoint.isAny());

    REQUIRE(Ipv4Endpoint::parse("0.0.0.0", 20777, endpoint));
    REQUIRE(endpoint.isAny());

    REQUIRE(Ipv4Endpoint::parse("192.168.1.40", 20777, endpoint));
    REQUIRE(endpoint.address() == std::array<std::uint8_t, 4>{192, 168, 1, 40});

    REQUIRE(Ipv4Endpoint::parse("255.255.255.255", 1, endpoint));
}

TEST_CASE("Una direccion invalida se rechaza en vez de adivinarse",
          "[net][endpoint][RNF-097]")
{
    Ipv4Endpoint endpoint;

    REQUIRE_FALSE(Ipv4Endpoint::parse("", 1, endpoint));
    REQUIRE_FALSE(Ipv4Endpoint::parse("127.0.0", 1, endpoint));
    REQUIRE_FALSE(Ipv4Endpoint::parse("127.0.0.1.5", 1, endpoint));
    REQUIRE_FALSE(Ipv4Endpoint::parse("127.0.0.256", 1, endpoint));
    REQUIRE_FALSE(Ipv4Endpoint::parse("127.0.0.-1", 1, endpoint));
    REQUIRE_FALSE(Ipv4Endpoint::parse("localhost", 1, endpoint));
    REQUIRE_FALSE(Ipv4Endpoint::parse("127.0.0.", 1, endpoint));
    REQUIRE_FALSE(Ipv4Endpoint::parse(".1.2.3", 1, endpoint));
    REQUIRE_FALSE(Ipv4Endpoint::parse("1.2.3.4444", 1, endpoint));
    REQUIRE_FALSE(Ipv4Endpoint::parse("1.2.3.0x4", 1, endpoint));
}

TEST_CASE("La conversion a orden de red y de vuelta es fiel", "[net][endpoint]")
{
    Ipv4Endpoint original;
    REQUIRE(Ipv4Endpoint::parse("192.168.1.40", 20777, original));

    const auto roundTrip =
        Ipv4Endpoint::fromNetworkOrder(original.toNetworkOrder(), original.port());

    REQUIRE(roundTrip == original);
}

TEST_CASE("El endpoint se muestra como lo espera un humano", "[net][endpoint]")
{
    Ipv4Endpoint endpoint;
    REQUIRE(Ipv4Endpoint::parse("127.0.0.1", 20777, endpoint));
    REQUIRE(endpoint.toString() == "127.0.0.1:20777");
}
