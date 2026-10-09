/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace ilemu {

// Architectural abort registers survive ordinary user/kernel transitions.
// They belong to the guest thread, independently of transient fault delivery.
struct CpuAbortState {
    std::uint32_t fault_status { };
    std::uint32_t fault_address { };
};

struct CpuThreadState {
    std::array<std::uint32_t, 16> registers { };
    std::array<std::uint32_t, 64> extension_registers { };
    std::uint32_t cpsr { };
    std::uint32_t fpscr { };
    std::optional<std::uint32_t> cthread_self;
    CpuAbortState abort_state;
};


}
