#include "pw/core/time.hpp"

namespace pw {

std::uint64_t monotonicNowNs() noexcept
{
    const auto since = MonotonicClock::now().time_since_epoch();
    const auto ns    = std::chrono::duration_cast<std::chrono::nanoseconds>(since).count();
    return static_cast<std::uint64_t>(ns);
}

std::int64_t wallClockNowMs() noexcept
{
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

} // namespace pw
