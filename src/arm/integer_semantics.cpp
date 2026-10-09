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
}
