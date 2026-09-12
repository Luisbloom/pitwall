// Pruebas del reloj monotono (F0-07).
#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <vector>

#include "pw/core/time.hpp"

TEST_CASE("El reloj del proyecto es monotono por construccion", "[core][time]")
{
    // La garantia la da el estandar: si steady_clock no fuera monotono, esta
    // comprobacion habria fallado ya en tiempo de compilacion al incluir la
    // cabecera. Se repite aqui para que la prueba diga QUE se esta asumiendo.
    STATIC_REQUIRE(pw::MonotonicClock::is_steady);
}

TEST_CASE("monotonicNowNs nunca decrece", "[core][time]")
{
    constexpr int kSamples = 200'000;

    auto previous = pw::monotonicNowNs();
    for (int i = 0; i < kSamples; ++i) {
        const auto current = pw::monotonicNowNs();
        REQUIRE(current >= previous);
        previous = current;
    }
}

TEST_CASE("monotonicNowNs avanza con el tiempo real", "[core][time]")
{
    const auto start = pw::monotonicNowNs();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const auto elapsed = pw::monotonicNowNs() - start;

    REQUIRE(elapsed >= 10'000'000ULL);      // al menos 10 ms
    REQUIRE(elapsed <  5'000'000'000ULL);   // y no un absurdo
}

TEST_CASE("El reloj es coherente entre hilos", "[core][time]")
{
    const auto           start = pw::monotonicNowNs();
    std::vector<std::uint64_t> samples(4);

    {
        std::vector<std::thread> threads;
        threads.reserve(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) {
            threads.emplace_back([&samples, i] { samples[i] = pw::monotonicNowNs(); });
        }
        for (auto& thread : threads) { thread.join(); }
    }

    for (const auto sample : samples) { REQUIRE(sample >= start); }
}

TEST_CASE("El reloj de pared existe y es plausible", "[core][time]")
{
    // Solo para mostrar y registrar; jamas para medir intervalos.
    const auto nowMs = pw::wallClockNowMs();
    REQUIRE(nowMs > 1'600'000'000'000LL);   // posterior a 2020
}
