/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"

namespace ilemu::arm {
void decode_t32_shifted_register(
    Instruction&, unsigned first, unsigned second) noexcept;
}
