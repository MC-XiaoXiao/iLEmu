/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/instruction_profile.hpp"

namespace ilemu::arm {
void decode_t32_modified_immediate(
    Instruction&, unsigned first, unsigned second,
    execution::ThumbArithmeticProfile) noexcept;
}
