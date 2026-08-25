#pragma once

#include <cstdint>

namespace ilemu::darwin::mach::clock {

// XNU 792.24.17 osfmk/mach/clock_types.h and kern/syscall_sw.c.
constexpr std::uint32_t sleep_trap = 62;
constexpr std::uint32_t null_clock_name = 0;
constexpr std::uint32_t system_clock_id = 0;
constexpr std::uint32_t calendar_clock_id = 1;
constexpr std::uint32_t time_absolute = 0;
constexpr std::uint32_t time_relative = 1;
constexpr std::uint32_t maximum_sleep_type = time_relative;
constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000ULL;
constexpr std::uint32_t timespec_seconds_offset = 0;
constexpr std::uint32_t timespec_nanoseconds_offset = sizeof(std::uint32_t);
constexpr std::uint32_t get_time_resolution_flavor = 1;
constexpr std::uint32_t alarm_current_resolution_flavor = 3;
constexpr std::uint32_t alarm_minimum_resolution_flavor = 4;
constexpr std::uint32_t alarm_maximum_resolution_flavor = 5;
constexpr std::uint32_t attribute_word_count = 1;
// VirtualClock advances in deterministic nanoseconds; ordinary execution
// samples it in one-microsecond quanta.
constexpr std::uint32_t virtual_resolution_nanoseconds = 1'000;

} // namespace ilemu::darwin::mach::clock
