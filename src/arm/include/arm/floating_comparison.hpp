/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
namespace ilemu::arm {
// Scalar comparisons ignore legacy vector length/stride and can raise only
// Invalid Operation or Input Denormal. Other trap controls are irrelevant.
inline constexpr std::uint32_t floating_comparison_controls = 0x00008100U;
struct FloatingComparisonResult {
    std::uint32_t nzcv, exceptions;
};
FloatingComparisonResult compare_floating(const FloatingComparisonOperands&,
    std::uint64_t first, std::uint64_t second, std::uint32_t fpscr) noexcept;
}
