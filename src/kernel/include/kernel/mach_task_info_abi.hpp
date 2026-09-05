#pragma once

#include <cstddef>
#include <cstdint>

namespace ilemu::darwin::mach::task_info {

// XNU 792.24.17 osfmk/mach/task_info.h. Fields are natural_t words at the
// 32-bit ARM compatibility boundary.
inline constexpr std::uint32_t absolute_time_flavor = 1;
inline constexpr std::size_t absolute_time_word_count = 8;

inline constexpr std::uint32_t events_flavor = 2;
inline constexpr std::size_t events_word_count = 8;

inline constexpr std::uint32_t thread_times_flavor = 3;
inline constexpr std::size_t thread_times_word_count = 4;

inline constexpr std::uint32_t basic_32_flavor = 4;
inline constexpr std::size_t basic_32_word_count = 8;

inline constexpr std::uint32_t basic_64_flavor = 5;
inline constexpr std::size_t basic_64_word_count = 10;

inline constexpr std::uint32_t timeshare_policy = 1;

} // namespace ilemu::darwin::mach::task_info
