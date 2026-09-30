// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Define guest ARM thread-state flavors and register layouts.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-4903.241.1/osfmk/mach/arm/thread_status.h

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ilemu::darwin::arm_thread {

// The iPhoneOS 1.0 ARM_THREAD_STATE flavor used by libSystem: r0-r15 followed
// by CPSR, matching the 17-natural state accepted by thread_create_running.
inline constexpr std::uint32_t general_state_flavor = 1;
inline constexpr std::size_t general_register_count = 16;
inline constexpr std::size_t cpsr_index = general_register_count;
inline constexpr std::size_t general_state_word_count =
    general_register_count + 1U;

// ARM_VFP_STATE exposes 32 D registers as 64 natural words, then FPSCR.
// Historical kernels also accept a 33-word prefix containing only D0-D15.
inline constexpr std::uint32_t floating_state_flavor = 2U;
inline constexpr std::size_t floating_register_words = 64U;
inline constexpr std::size_t floating_fpscr_index = floating_register_words;
inline constexpr std::size_t floating_state_word_count = 65U;
inline constexpr std::size_t floating_prefix_word_count = 33U;
inline constexpr std::size_t maximum_state_word_count = 144U;

using GeneralState = std::array<std::uint32_t, general_state_word_count>;

// arm/status.c machine_thread_set_state preserves PSR_USER_MASK: the
// asynchronous-abort, IRQ/FIQ masks and mode belong to the target thread.
// NZCV, Q, GE and Thumb state remain writable by the guest.
inline constexpr std::uint32_t privileged_cpsr_mask = 0x1dfU;
inline constexpr std::uint32_t user_cpsr = 0x10U;
constexpr std::uint32_t restored_cpsr(
    std::uint32_t requested, std::uint32_t current)
{
    return (requested & ~privileged_cpsr_mask) |
           (current & privileged_cpsr_mask);
}

} // namespace ilemu::darwin::arm_thread
