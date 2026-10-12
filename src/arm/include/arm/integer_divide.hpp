/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"

namespace ilemu::arm {
// IntegerDivide uses rn/rm for dividend/divisor and sign_extend for signedness.
std::uint32_t integer_divide(const Instruction&, std::uint32_t dividend,
    std::uint32_t divisor) noexcept;
}
