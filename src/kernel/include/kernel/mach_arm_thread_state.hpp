// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "kernel/mach_arm_thread_abi.hpp"
#include <algorithm>
#include <span>

namespace ilemu::darwin::arm_thread {

// Transfer only the defined register prefix; wire padding stays in the server.
template <typename Processor>
bool read_state(const Processor& cpu, std::uint32_t flavor,
    std::span<std::uint32_t> state)
{
    if (flavor == general_state_flavor && state.size() == general_state_word_count) {
        std::copy(cpu.registers().begin(), cpu.registers().end(), state.begin());
        state[cpsr_index] = cpu.cpsr();
        return true;
    }
    if (flavor != floating_state_flavor ||
        (state.size() != floating_prefix_word_count - 1U &&
            state.size() != floating_state_word_count))
        return false;
    const auto registers = std::min(state.size(), floating_register_words);
    std::copy_n(cpu.extension_registers().begin(), registers, state.begin());
    if (state.size() == floating_state_word_count)
        state[floating_fpscr_index] = cpu.fpscr();
    return true;
}

template <typename Processor>
bool write_state(Processor& cpu, std::uint32_t flavor,
    std::span<const std::uint32_t> state)
{
    if (flavor == general_state_flavor && state.size() == general_state_word_count) {
        std::copy_n(state.begin(), general_register_count, cpu.registers().begin());
        cpu.set_cpsr(restored_cpsr(state[cpsr_index], cpu.cpsr()));
        return true;
    }
    if (flavor != floating_state_flavor ||
        (state.size() != floating_prefix_word_count - 1U &&
            state.size() != floating_state_word_count))
        return false;
    const auto registers = std::min(state.size(), floating_register_words);
    std::copy_n(state.begin(), registers, cpu.extension_registers().begin());
    // Native short setters read word64 outside their declared MIG payload.
    // Preserve the unspecified FPSCR instead of reading beyond the request.
    if (state.size() == floating_state_word_count)
        cpu.set_fpscr(state[floating_fpscr_index]);
    return true;
}

} // namespace ilemu::darwin::arm_thread
