/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
namespace ilemu::arm {
// Exception trapping needs an architectural trap delivery interface. Until
// available, executors expose these states as an explicit unsupported exit.
inline constexpr std::uint32_t floating_exception_enables = 0x00009f00U;
struct FloatingConversionResult {
    std::uint64_t value;
    std::uint32_t exceptions;
};
FloatingConversionResult convert_floating(const FloatingConversionOperands&,
    std::uint64_t input, std::uint32_t fpscr) noexcept;
}
