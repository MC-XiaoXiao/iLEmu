/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/multiply.hpp"
#include <bit>
#include <limits>

namespace ilemu::arm {
std::uint64_t multiply(const Instruction& inst, std::uint32_t first,
    std::uint32_t second, std::uint32_t low, std::uint32_t high) noexcept
{
    using Operation = MultiplyOperation;
    const auto operation = static_cast<Operation>(inst.opcode);
    if (operation == Operation::WordHalfword) {
        const auto half = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(
            second >> ((inst.shift_amount & 2U) != 0 ? 16U : 0U)));
        const auto product = std::int64_t { std::bit_cast<std::int32_t>(first) } * half;
        const auto value = (product >> 16U) +
            (inst.accumulate ? std::bit_cast<std::int32_t>(high) : 0);
        return std::bit_cast<std::uint64_t>(value);
    }
    if (operation == Operation::Halfword || operation == Operation::HalfwordLong) {
        const auto half = [](std::uint32_t value, bool upper) {
            return std::bit_cast<std::int16_t>(
                static_cast<std::uint16_t>(value >> (upper ? 16U : 0U)));
        };
        const auto product =
            std::int64_t { half(first, (inst.shift_amount & 1U) != 0) } *
            half(second, (inst.shift_amount & 2U) != 0);
        if (operation == Operation::HalfwordLong)
            return static_cast<std::uint64_t>(product) +
                ((std::uint64_t { high } << 32U) | low);
        return inst.accumulate
            ? std::bit_cast<std::uint64_t>(product + std::bit_cast<std::int32_t>(high))
            : static_cast<std::uint32_t>(product);
    }
    const auto product = inst.sign_extend
        ? static_cast<std::uint64_t>(std::int64_t { std::bit_cast<std::int32_t>(first) } *
                                    std::bit_cast<std::int32_t>(second))
        : std::uint64_t { first } * second;
    switch (operation) {
    case Operation::Halfword:
    case Operation::HalfwordLong:
    case Operation::WordHalfword: return 0; // Handled before full-word multiplication.
    case Operation::Word:
        return static_cast<std::uint32_t>(product + (inst.accumulate ? high : 0U));
    case Operation::Subtract:
        return static_cast<std::uint32_t>(high - product);
    case Operation::Long:
        // Accumulate unsigned bit patterns so signed overflow is never used.
        return product + (inst.accumulate ? (std::uint64_t { high } << 32U) | low : 0U);
    case Operation::DualAccumulateLong:
        return product + low + high;
    case Operation::High:
    case Operation::HighSubtract: {
        const auto addend = inst.accumulate ? std::uint64_t { high } << 32U : 0U;
        const auto value = operation == Operation::HighSubtract
            ? addend - product : addend + product;
        return static_cast<std::uint32_t>((value +
            (inst.shift_amount != 0 ? 0x80000000ULL : 0U)) >> 32U);
    }
    }
    return 0;
}
std::uint32_t multiply_nz(const Instruction& inst, std::uint32_t cpsr,
    std::uint64_t value) noexcept
{
    const auto top = static_cast<std::uint32_t>(
        multiply_writes_pair(inst) ? value >> 32U : value);
    return (cpsr & 0x3fffffffU) | (top & 0x80000000U) |
           (value == 0 ? 0x40000000U : 0U);
}
std::uint32_t multiply_q(std::uint32_t cpsr, std::uint64_t wide_sum) noexcept
{
    const auto value = std::bit_cast<std::int64_t>(wide_sum);
    return value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()
        ? cpsr | (1U << 27U) : cpsr;
}
}
