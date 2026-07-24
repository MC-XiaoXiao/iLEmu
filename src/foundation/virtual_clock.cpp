#include "ilegacysim/virtual_clock.hpp"

#include <algorithm>
#include <limits>

namespace ilegacysim {
namespace {

constexpr std::uint64_t maximum_positive_offset =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

std::int64_t calendar_offset(std::uint64_t calendar_time,
                             std::uint64_t monotonic_time) {
    if (calendar_time >= monotonic_time) {
        return static_cast<std::int64_t>(
            std::min(calendar_time - monotonic_time, maximum_positive_offset));
    }
    const auto magnitude = monotonic_time - calendar_time;
    if (magnitude > maximum_positive_offset) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
}

}  // namespace

VirtualClock::VirtualClock(std::uint64_t initial_time)
    : now_{initial_time},
      wall_time_offset_{calendar_offset(
          default_wall_time_epoch_seconds * nanoseconds_per_second,
          initial_time)} {}

std::uint64_t VirtualClock::now() const {
    return now_.load(std::memory_order_relaxed);
}

std::uint64_t VirtualClock::wall_time() const {
    const auto monotonic_time = now();
    const auto offset = wall_time_offset_.load(std::memory_order_relaxed);
    if (offset < 0) {
        const auto magnitude =
            offset == std::numeric_limits<std::int64_t>::min()
                ? maximum_positive_offset + 1U
                : static_cast<std::uint64_t>(-offset);
        return monotonic_time > magnitude ? monotonic_time - magnitude : 0;
    }
    const auto positive_offset = static_cast<std::uint64_t>(offset);
    if (monotonic_time >
        std::numeric_limits<std::uint64_t>::max() - positive_offset) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return positive_offset + monotonic_time;
}

void VirtualClock::set_wall_time(std::uint64_t unix_time_nanoseconds) {
    wall_time_offset_.store(calendar_offset(unix_time_nanoseconds, now()),
                            std::memory_order_relaxed);
}

void VirtualClock::synchronize_wall_time(std::uint64_t unix_time_nanoseconds) {
    set_wall_time(unix_time_nanoseconds);
}

std::uint64_t VirtualClock::tick(std::uint64_t increment) {
    return now_.fetch_add(increment, std::memory_order_relaxed) + increment;
}

void VirtualClock::advance_to(std::uint64_t deadline) {
    auto current = now();
    while (current < deadline &&
           !now_.compare_exchange_weak(current, deadline,
                                       std::memory_order_relaxed)) {
    }
}

}  // namespace ilegacysim
