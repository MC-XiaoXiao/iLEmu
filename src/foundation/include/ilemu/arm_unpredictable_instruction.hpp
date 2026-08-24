#pragma once

#include <cstdint>
#include <span>

#include "ilemu/arm_cpu_model.hpp"

namespace ilemu {

// Applies the selected core's deterministic behavior for an instruction that
// Dynarmic reports as unpredictable. Thumb-2 instructions use the decoder
// order `first_halfword << 16 | second_halfword`; ARM instructions use their
// architectural 32-bit encoding. Returns false when the strict exception path
// must remain in effect.
[[nodiscard]] bool emulate_arm_unpredictable_instruction(
    ArmUnpredictableInstructionProfile profile, bool thumb,
    std::uint32_t instruction,
    std::span<std::uint32_t, 64> extension_registers) noexcept;

} // namespace ilemu
