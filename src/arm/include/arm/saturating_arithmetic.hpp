/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/integer_semantics.hpp"

namespace ilemu::arm {
// Rn is the first source; Rm is the second source for binary operations.
// Immediate names saturation width, including zero for unsigned saturation.
enum class SaturatingOperation {
    Signed,
    Unsigned,
    SignedHalfwords,
    UnsignedHalfwords,
    Add,
    Subtract,
    DoubleAdd,
    DoubleSubtract
};
AluResult saturating_arithmetic(const Instruction&, std::uint32_t first,
    std::uint32_t second, std::uint32_t cpsr) noexcept;
}
