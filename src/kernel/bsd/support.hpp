#pragma once

#include "ilemu/hfs_metadata.hpp"
#include "ilemu/virtual_clock.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <system_error>

namespace ilemu::bsd_support {

inline constexpr std::uint32_t carry_flag = 1U << 29U;
inline constexpr std::uint32_t bad_file_descriptor = 9;
inline constexpr std::uint32_t bad_address = 14;
inline constexpr std::uint32_t invalid_argument = 22;
inline constexpr std::uint32_t would_block = 35;
inline constexpr std::uint32_t address_in_use = 48;
inline constexpr std::uint32_t already_connected = 56;
inline constexpr std::uint32_t not_connected = 57;
inline constexpr std::uint32_t connection_refused = 61;
inline constexpr std::uint32_t not_implemented = 78;
inline constexpr std::uint32_t maximum_socket_address_size = 255;
inline constexpr std::size_t maximum_io = 16U * 1024U * 1024U;

[[nodiscard]] hfs::Timestamp
guest_filesystem_timestamp(const VirtualClock &clock);

[[nodiscard]] std::uint32_t
darwin_filesystem_error(const std::error_code &error,
                        std::uint32_t fallback = 5U);

[[nodiscard]] std::string
format_payload_prefix(std::span<const std::byte> bytes);

} // namespace ilemu::bsd_support
