#pragma once

#include <cstdint>

namespace ilemu::darwin::mach::scheduler {

// XNU 792.24.17 osfmk/mach/thread_switch.h.
constexpr std::uint32_t swtch_pri_trap = 59;
constexpr std::uint32_t swtch_trap = 60;
constexpr std::uint32_t thread_switch_trap = 61;
constexpr std::uint32_t switch_option_none = 0;
constexpr std::uint32_t switch_option_depress = 1;
constexpr std::uint32_t switch_option_wait = 2;
constexpr std::uint32_t maximum_switch_option = switch_option_wait;

constexpr std::uint64_t nanoseconds_per_millisecond = 1'000'000ULL;

} // namespace ilemu::darwin::mach::scheduler
