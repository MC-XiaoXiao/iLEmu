/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/instruction_profile.hpp"
namespace ilemu::arm {
bool decode_simd_conversion(Instruction&, std::uint32_t, bool thumb,
    execution::VfpRegisterProfile, execution::ArmInstructionProfile,
    execution::AdvancedSimdProfile) noexcept;
}
