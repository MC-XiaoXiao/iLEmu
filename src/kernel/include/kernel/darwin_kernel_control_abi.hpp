#pragma once

#include <cstddef>
#include <cstdint>

namespace ilemu::darwin::kernel_control {

// XNU 792 bsd/sys/sys_domain.h and bsd/sys/kern_control.h.
inline constexpr std::uint32_t protocol_family_system = 32;
inline constexpr std::uint32_t protocol_event = 1;
inline constexpr std::uint32_t protocol_control = 2;
inline constexpr std::uint16_t system_address_control = 2;

inline constexpr std::uint32_t maximum_name_size = 96;
inline constexpr std::uint32_t control_info_size =
    sizeof(std::uint32_t) + maximum_name_size;
inline constexpr std::uint32_t control_info_identifier_offset = 0;
inline constexpr std::uint32_t control_info_name_offset = 4;

inline constexpr std::uint32_t socket_address_size = 32;
inline constexpr std::uint32_t socket_address_length_offset = 0;
inline constexpr std::uint32_t socket_address_family_offset = 1;
inline constexpr std::uint32_t socket_address_system_offset = 2;
inline constexpr std::uint32_t socket_address_identifier_offset = 4;
inline constexpr std::uint32_t socket_address_unit_offset = 8;
inline constexpr std::uint32_t socket_address_reserved_offset = 12;
inline constexpr std::uint32_t socket_address_reserved_count = 5;

// _IOWR('N', 3, struct ctl_info), where sizeof(ctl_info) == 100 on ARM32.
inline constexpr std::uint32_t ioctl_get_info = 0xc0644e03U;

} // namespace ilemu::darwin::kernel_control
