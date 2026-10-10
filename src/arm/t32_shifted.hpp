/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/instruction_profile.hpp"

namespace ilemu::arm {
void decode_t32_shifted_register(
    Instruction&, unsigned first, unsigned second,
    execution::ThumbArithmeticProfile) noexcept;
void decode_t32_register_shift(Instruction&, unsigned first, unsigned second,
    execution::ThumbArithmeticProfile) noexcept;
}
