/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/instruction_profile.hpp"
namespace ilemu::arm {
void decode_t32_plain_arithmetic(Instruction&, unsigned first, unsigned second,
    execution::ThumbArithmeticProfile) noexcept;
}
