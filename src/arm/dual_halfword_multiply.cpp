/* SPDX-License-Identifier: MPL-2.0 */
#include "dual_halfword_multiply.hpp"
#include <bit>
namespace ilemu::arm {
MultiplyResult dual_halfword_multiply(const Instruction& inst, std::uint32_t first,
    std::uint32_t second, std::uint32_t low, std::uint32_t high,
    std::uint32_t cpsr) noexcept
{
    using Op = MultiplyOperation;
    const auto operation = static_cast<Op>(inst.opcode);
    if (inst.exchange)
        second = std::rotl(second, 16);
    const auto half = [](std::uint32_t word, unsigned shift) {
        return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(word >> shift));
    };
    const auto first_product = std::int64_t {half(first, 0)} * half(second, 0);
    const auto second_product = std::int64_t {half(first, 16)} * half(second, 16);
    const bool subtract = operation == Op::DualHalfwordSubtract ||
                          operation == Op::DualHalfwordLongSubtract;
    const auto product = subtract ? first_product - second_product
                                  : first_product + second_product;
    if (multiply_writes_pair(operation))
        return {static_cast<std::uint64_t>(product) +
            ((std::uint64_t {high} << 32U) | low), cpsr};
    // Q describes the complete wide result, including cancellation by the
    // accumulator. An overflowing intermediate alone must not set it.
    const auto sum = product + (inst.accumulate ? std::bit_cast<std::int32_t>(high) : 0);
    const auto wide = std::bit_cast<std::uint64_t>(sum);
    return {static_cast<std::uint32_t>(wide), multiply_q(cpsr, wide)};
}
}
