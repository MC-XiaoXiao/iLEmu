#pragma once

#include <cstdint>

namespace ilemu::darwin::kqueue {

inline constexpr std::int16_t filter_read = -1;
inline constexpr std::int16_t filter_write = -2;

inline constexpr std::uint16_t event_add = 0x0001;
inline constexpr std::uint16_t event_delete = 0x0002;

namespace arm32_event {
inline constexpr std::uint32_t identifier_offset = 0;
inline constexpr std::uint32_t filter_offset = 4;
inline constexpr std::uint32_t flags_offset = 6;
inline constexpr std::uint32_t filter_flags_offset = 8;
inline constexpr std::uint32_t data_offset = 12;
inline constexpr std::uint32_t user_data_offset = 16;
inline constexpr std::uint32_t size = 20;
}  // namespace arm32_event

namespace arm32_timespec {
inline constexpr std::uint32_t seconds_offset = 0;
inline constexpr std::uint32_t nanoseconds_offset = 4;
inline constexpr std::uint32_t size = 8;
}  // namespace arm32_timespec

inline constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000ULL;

}  // namespace ilemu::darwin::kqueue
