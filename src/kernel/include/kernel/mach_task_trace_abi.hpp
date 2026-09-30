// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>

namespace ilemu::darwin::task_trace {
// XNU task_info.h, atm.c and atm_notification.defs. The 64-bit fields
// are four-byte aligned in the ARM32 MIG request and notification.
inline constexpr std::uint32_t information_flavor = 24;
inline constexpr std::uint32_t information_count = 6;
inline constexpr std::uint32_t inspect_syscall = 477;
inline constexpr std::uint32_t notification_port = 21;
inline constexpr std::uint32_t inspect_message = 11501;
inline constexpr std::uint32_t inspect_message_size = 68;
inline constexpr std::uint64_t maximum_buffer_size = 0x40000000;
inline constexpr std::uint64_t maximum_mailbox_size = 8 * 4096;
} // namespace ilemu::darwin::task_trace
