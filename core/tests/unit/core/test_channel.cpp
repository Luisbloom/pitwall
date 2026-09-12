// Pruebas de Channel<T> — la garantia central del proyecto (ADR-009, RN-001).
#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <type_traits>

#include "pw/core/channel.hpp"

using pw::Channel;
using pw::Provenance;
using pw::Reason;

TEST_CASE("Channel exige comprobar known() antes de usar el valor", "[core][channel][ADR-009]")
{
    const auto restricted = Channel<float>::restricted();

    REQUIRE_FALSE(restricted.known());
    REQUIRE(restricted.provenance() == Provenance::Restricted);
    REQUIRE(restricted.reason() == Reason::RestrictedByOwner);
    REQUIRE(restricted.valueOr(-1.0F) == -1.0F);
    REQUIRE_THROWS_AS(restricted.value(), pw::ChannelAccessError);
}

TEST_CASE("Channel no se convierte implicitamente a su valor", "[core][channel][ADR-009]")
{
    // La prueba complementaria, que verifica que el uso real NO COMPILA, la
    // ejecuta CTest: nocompile.channel_implicit_conversion.
    STATIC_REQUIRE_FALSE(std::is_convertible_v<Channel<float>, float>);
    STATIC_REQUIRE_FALSE(std::is_convertible_v<Channel<bool>, bool>);
    STATIC_REQUIRE_FALSE(std::is_convertible_v<Channel<int>, int>);
    STATIC_REQUIRE_FALSE(std::is_constructible_v<float, Channel<float>>);
}

TEST_CASE("Un canal recien construido no tiene valor y lo explica", "[core][channel]")
{
    const Channel<int> pending;

    REQUIRE_FALSE(pending.known());
    REQUIRE(pending.provenance() == Provenance::Unavailable);
    REQUIRE(pending.reason() == Reason::NotYetReceived);
}

TEST_CASE("Las cinco procedencias se construyen y se distinguen", "[core][channel][RF-612]")
{
    const auto raw       = Channel<float>::raw(312.4F);
    const auto derived   = Channel<float>::derived(1.25F);
    const auto estimated = Channel<float>::estimated(0.42F, 0.1F);
    const auto missing   = Channel<float>::unavailable(Reason::NotSupportedByFormat);
    const auto blocked   = Channel<float>::restricted();

    REQUIRE(raw.known());
    REQUIRE(raw.provenance() == Provenance::Raw);
    REQUIRE(raw.value() == 312.4F);

    REQUIRE(derived.provenance() == Provenance::Derived);
    REQUIRE(derived.value() == 1.25F);

    REQUIRE(estimated.provenance() == Provenance::Estimated);
    REQUIRE(estimated.uncertainty() == 0.1F);

    REQUIRE_FALSE(missing.known());
    REQUIRE(missing.provenance() == Provenance::Unavailable);
    REQUIRE(missing.reason() == Reason::NotSupportedByFormat);

    REQUIRE_FALSE(blocked.known());
    REQUIRE(blocked.provenance() == Provenance::Restricted);
}

TEST_CASE("Un canal restringido nunca se lee como cero", "[core][channel][RN-001]")
{
    // El defecto clasico del ecosistema: pintar «0 % de desgaste» de un rival
    // cuya telemetria esta restringida. Aqui es imposible sin decidirlo a mano.
    const auto wear = Channel<float>::restricted();

    REQUIRE_FALSE(wear.known());
    REQUIRE(wear.provenance() != Provenance::Raw);
    REQUIRE(wear.valueOr(0.0F) == 0.0F);   // solo si quien llama lo pide
}

TEST_CASE("Un canal con valor no arrastra razon de ausencia", "[core][channel]")
{
    const auto speed = Channel<float>::raw(280.0F);

    REQUIRE(speed.known());
    REQUIRE(speed.reason() == Reason::None);
    REQUIRE(speed.uncertainty() == 0.0F);
}

TEST_CASE("Dos canales equivalentes son iguales", "[core][channel]")
{
    REQUIRE(Channel<int>::raw(7) == Channel<int>::raw(7));
    REQUIRE_FALSE(Channel<int>::raw(7) == Channel<int>::derived(7));
    REQUIRE_FALSE(Channel<int>::raw(7) == Channel<int>::raw(8));
    REQUIRE(Channel<int>::restricted() == Channel<int>::restricted());
    REQUIRE_FALSE(Channel<int>::restricted() ==
                  Channel<int>::unavailable(Reason::NotSupportedByFormat));
}

TEST_CASE("Los canales son utilizables en tiempo de compilacion", "[core][channel]")
{
    constexpr auto gear = Channel<int>::raw(4);
    STATIC_REQUIRE(gear.known());
    STATIC_REQUIRE(gear.valueOr(0) == 4);
    STATIC_REQUIRE(gear.provenance() == Provenance::Raw);
}

TEST_CASE("Los nombres de procedencia son estables", "[core][channel][RF-612]")
{
    REQUIRE(std::string_view{pw::provenanceName(Provenance::Raw)} == "raw");
    REQUIRE(std::string_view{pw::provenanceName(Provenance::Derived)} == "derived");
    REQUIRE(std::string_view{pw::provenanceName(Provenance::Estimated)} == "estimated");
    REQUIRE(std::string_view{pw::provenanceName(Provenance::Unavailable)} == "unavailable");
    REQUIRE(std::string_view{pw::provenanceName(Provenance::Restricted)} == "restricted");
}

TEST_CASE("Toda razon de ausencia tiene nombre y explicacion", "[core][channel]")
{
    constexpr Reason kAll[] = {Reason::None,
                               Reason::NotYetReceived,
                               Reason::NotSupportedByFormat,
                               Reason::NotApplicable,
                               Reason::RestrictedByOwner,
                               Reason::SentinelValue,
                               Reason::DecodeError,
                               Reason::Unknown};

    for (const auto reason : kAll) {
        REQUIRE(std::string_view{pw::reasonName(reason)}.empty() == false);
        REQUIRE(std::string_view{pw::reasonDescription(reason)}.empty() == false);
    }
}
