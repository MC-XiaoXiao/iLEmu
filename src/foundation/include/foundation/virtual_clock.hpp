#pragma once

#include <atomic>
#include <cstdint>

namespace ilemu {

// Process-shared hardware clock model. The monotonic counter drives scheduler
// waits and never changes discontinuously; the calendar is a separately
// adjustable RTC offset that advances from the same counter.
class VirtualClock {
public:
    static constexpr std::uint64_t default_initial_time = 1'000'000;
    static constexpr std::uint64_t default_wall_time_epoch_seconds =
        1'180'000'000;
    static constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000;

    explicit VirtualClock(std::uint64_t initial_time = default_initial_time);

    [[nodiscard]] std::uint64_t now() const;
    [[nodiscard]] std::uint64_t wall_time() const;
    void set_wall_time(std::uint64_t unix_time_nanoseconds);
    // Retained for deterministic fixtures that seed a known calendar value.
    void synchronize_wall_time(std::uint64_t unix_time_nanoseconds);
    std::uint64_t tick(std::uint64_t increment);
    void advance_to(std::uint64_t deadline);

private:
    std::atomic_uint64_t now_;
    std::atomic_int64_t wall_time_offset_;
};

} // namespace ilemu
