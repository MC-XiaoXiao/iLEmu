/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/floating_conversion.hpp"
namespace ilemu::arm {
// Legacy VFP short-vector length/stride are not scalar operations.
inline constexpr std::uint32_t floating_vector_controls = 0x00370000U;
constexpr bool floating_is_bitwise(FloatingArithmeticOperation op) noexcept
{
    return op == FloatingArithmeticOperation::Move ||
           op == FloatingArithmeticOperation::Absolute ||
           op == FloatingArithmeticOperation::Negate;
}
constexpr std::uint32_t floating_arithmetic_controls(
    FloatingArithmeticOperation op) noexcept
{
    return floating_vector_controls |
           (floating_is_bitwise(op) ? 0U : floating_exception_enables);
}
struct FloatingArithmeticResult {
    std::uint64_t value;
    std::uint32_t exceptions;
};
FloatingArithmeticResult calculate_floating(const FloatingArithmeticOperands&,
    std::uint64_t first, std::uint64_t second, std::uint64_t accumulator,
    std::uint32_t fpscr) noexcept;
}
