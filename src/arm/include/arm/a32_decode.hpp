/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/instruction_profile.hpp"
namespace ilemu::arm {
Instruction decode_a32(std::uint32_t word, execution::VfpRegisterProfile =
    execution::VfpRegisterProfile::D32, execution::ArmInstructionProfile =
    execution::ArmInstructionProfile::Armv6T2, execution::VfpInstructionProfile =
    execution::VfpInstructionProfile::Vfpv3,
    execution::IntegerDivideProfile = execution::IntegerDivideProfile::None) noexcept;
}
