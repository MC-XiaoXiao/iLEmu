/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/instruction_profile.hpp"

namespace ilemu::arm {
void decode_t32_transfer(Instruction&, unsigned first, unsigned second,
    bool last_in_it, execution::ArmInstructionProfile) noexcept;
}
