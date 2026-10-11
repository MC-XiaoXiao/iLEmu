/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/integer_semantics.hpp"
#include <bit>

namespace ilemu::arm {
bool condition_passed(unsigned condition, std::uint32_t cpsr) noexcept
{
    const bool n = (cpsr >> 31) != 0, z = (cpsr & (1U << 30)) != 0;
    const bool c = (cpsr & (1U << 29)) != 0, v = (cpsr & (1U << 28)) != 0;
    switch (condition) {
    case 0:
        return z;
    case 1:
        return !z;
    case 2:
        return c;
    case 3:
        return !c;
    case 4:
        return n;
    case 5:
        return !n;
    case 6:
        return v;
    case 7:
        return !v;
    case 8:
        return c && !z;
    case 9:
        return !c || z;
    case 10:
        return n == v;
    case 11:
        return n != v;
    case 12:
        return !z && n == v;
    case 13:
        return z || n != v;
    case 14:
        return true;
    default:
        return false;
    }
}

ShiftResult shift(std::uint32_t value, ShiftKind kind, unsigned amount,
    bool carry, bool register_amount) noexcept
{
    if (register_amount && amount == 0)
        return { value, carry };
    switch (kind) {
    case ShiftKind::Lsl:
        if (amount == 0)
            return { value, carry };
        if (amount < 32)
            return { value << amount, ((value >> (32 - amount)) & 1U) != 0 };
        return { 0, amount == 32 && (value & 1U) != 0 };
    case ShiftKind::Lsr:
        if (amount == 0)
            amount = 32;
        if (amount < 32)
            return { value >> amount, ((value >> (amount - 1)) & 1U) != 0 };
        return { 0, amount == 32 && (value >> 31) != 0 };
    case ShiftKind::Asr: {
        if (amount == 0)
            amount = 32;
        const bool sign = (value >> 31) != 0;
        if (amount >= 32)
            return { sign ? 0xffffffffU : 0, sign };
        const auto fill = sign ? (~std::uint32_t { 0 } << (32 - amount)) : 0;
        return { (value >> amount) | fill,
            ((value >> (amount - 1)) & 1U) != 0 };
    }
    case ShiftKind::Ror:
        if (amount == 0)
            return { (value >> 1) | (std::uint32_t { carry } << 31),
                (value & 1U) != 0 };
        value = std::rotr(value, static_cast<int>(amount & 31U));
        return { value, (value >> 31) != 0 };
    }
    return { value, carry };
}

ArithmeticResult add_with_carry(
    std::uint32_t a, std::uint32_t b, bool carry) noexcept
{
    const auto wide = std::uint64_t { a } + b + std::uint32_t { carry };
    const auto value = static_cast<std::uint32_t>(wide);
    return { value, (wide >> 32) != 0,
        ((a ^ value) & (b ^ value) & 0x80000000U) != 0 };
}
std::uint32_t set_nz(std::uint32_t cpsr, std::uint32_t value) noexcept
{
    return (cpsr & 0x3fffffffU) | (value & 0x80000000U) |
           (value == 0 ? 0x40000000U : 0);
}
AluResult calculate_alu(unsigned opcode, bool set_flags, std::uint32_t a,
    ShiftResult shifted, std::uint32_t cpsr) noexcept
{
    const bool carry = (cpsr & (1U << 29)) != 0;
    const auto b = shifted.value;
    std::uint32_t value = 0;
    arm::ArithmeticResult arithmetic { };
    bool arithmetic_flags = false;
    switch (opcode) {
    case 0:
    case 8:
        value = a & b;
        break;
    case 1:
    case 9:
        value = a ^ b;
        break;
    case 2:
    case 10:
        arithmetic = arm::add_with_carry(a, ~b, true);
        arithmetic_flags = true;
        break;
    case 3:
        arithmetic = arm::add_with_carry(b, ~a, true);
        arithmetic_flags = true;
        break;
    case 4:
    case 11:
        arithmetic = arm::add_with_carry(a, b, false);
        arithmetic_flags = true;
        break;
    case 5:
        arithmetic = arm::add_with_carry(a, b, carry);
        arithmetic_flags = true;
        break;
    case 6:
        arithmetic = arm::add_with_carry(a, ~b, carry);
        arithmetic_flags = true;
        break;
    case 7:
        arithmetic = arm::add_with_carry(b, ~a, carry);
        arithmetic_flags = true;
        break;
    case 12:
        value = a | b;
        break;
    case 13:
        value = b;
        break;
    case 14:
        value = a & ~b;
        break;
    case 15:
        value = ~b;
        break;
    case arm::opcode_orn:
        value = a | ~b;
        break;
    }
    if (arithmetic_flags)
        value = arithmetic.value;
    if (set_flags) {
        cpsr = arm::set_nz(cpsr, value);
        cpsr = (cpsr & ~(1U << 29)) |
               (std::uint32_t {
                    arithmetic_flags ? arithmetic.carry : shifted.carry }
                   << 29);
        if (arithmetic_flags)
            cpsr = (cpsr & ~(1U << 28)) |
                   (std::uint32_t { arithmetic.overflow } << 28);
    }
    return { value, cpsr };
}

}
