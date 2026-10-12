/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/multiply.hpp"
#include <bit>

namespace ilemu::arm {
std::uint64_t multiply(const Instruction& inst, std::uint32_t first,
    std::uint32_t second, std::uint32_t low, std::uint32_t high) noexcept
{
    using Operation = MultiplyOperation;
    const auto operation = static_cast<Operation>(inst.opcode);
    const auto product = inst.sign_extend
        ? static_cast<std::uint64_t>(std::int64_t { std::bit_cast<std::int32_t>(first) } *
                                    std::bit_cast<std::int32_t>(second))
        : std::uint64_t { first } * second;
    switch (operation) {
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
}
