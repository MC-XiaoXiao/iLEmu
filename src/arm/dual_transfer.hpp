/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/instruction_profile.hpp"

namespace ilemu::arm {
bool decode_dual_transfer(Instruction&, std::uint32_t word, bool thumb,
    execution::ArmInstructionProfile) noexcept;
}
