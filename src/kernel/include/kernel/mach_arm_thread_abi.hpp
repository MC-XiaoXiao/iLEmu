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

using GeneralState = std::array<std::uint32_t, general_state_word_count>;

} // namespace ilemu::darwin::arm_thread
