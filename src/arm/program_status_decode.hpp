/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/instruction_profile.hpp"
namespace ilemu::arm {
bool decode_program_status(Instruction&, std::uint32_t, bool thumb,
    execution::ArmInstructionProfile) noexcept;
}
