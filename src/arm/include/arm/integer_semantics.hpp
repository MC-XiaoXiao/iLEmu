/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/a32_decode.hpp"
#include <cstdint>

namespace ilemu::arm {
bool condition_passed(unsigned condition, std::uint32_t cpsr) noexcept;
struct ShiftResult {
    std::uint32_t value;
    bool carry;
};
ShiftResult shift(std::uint32_t value, ShiftKind kind, unsigned amount,
    bool carry, bool register_amount) noexcept;
struct ArithmeticResult {
    std::uint32_t value;
    bool carry, overflow;
};
ArithmeticResult add_with_carry(
    std::uint32_t a, std::uint32_t b, bool carry) noexcept;
std::uint32_t set_nz(std::uint32_t cpsr, std::uint32_t value) noexcept;
struct AluResult {
    std::uint32_t value, cpsr;
};
AluResult calculate_alu(unsigned opcode, bool set_flags, std::uint32_t first,
    ShiftResult operand, std::uint32_t cpsr) noexcept;

}
