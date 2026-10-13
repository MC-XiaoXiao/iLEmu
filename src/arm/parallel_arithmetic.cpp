/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/parallel_arithmetic.hpp"
#include "parallel_arithmetic_decode.hpp"
#include <bit>
namespace ilemu::arm {
bool decode_parallel_arithmetic(Instruction& out, std::uint32_t word,
    bool thumb, execution::ArmInstructionProfile architecture) noexcept
{
    unsigned n, d, m = word & 15U;
    int operation;
    bool signed_lanes = false;
    if (thumb) {
        if ((word & 0xfff0f0f0U) == 0xfaa0f080U)
            operation = 6;
        else {
            if ((word & 0xff80f0b0U) != 0xfa80f000U)
                return false;
            constexpr int operations[] = {4, 0, 1, -1, 5, 3, 2, -1};
            operation = operations[(word >> 20U) & 7U];
            signed_lanes = (word & 0x40U) == 0;
        }
        n = (word >> 16U) & 15U;
        d = (word >> 8U) & 15U;
        if (!execution::has_armv6t2_extensions(architecture) ||
            n == 13 || d == 13 || m == 13)
            return true;
    } else {
        if (out.condition == 15)
            return false;
        if ((word & 0x0ff00ff0U) == 0x06800fb0U)
            operation = 6;
        else {
            if ((word & 0x0fb00f10U) != 0x06100f10U)
                return false;
            constexpr int operations[] = {0, 1, 2, 3, 4, -1, -1, 5};
            operation = operations[(word >> 5U) & 7U];
            signed_lanes = (word & 0x00400000U) == 0;
        }
        n = (word >> 16U) & 15U;
        d = (word >> 12U) & 15U;
    }
    if (operation < 0 || n == 15 || d == 15 || m == 15)
        return true;
    out.kind = InstructionKind::ParallelArithmetic;
    out.opcode = static_cast<unsigned>(operation);
    out.sign_extend = signed_lanes;
    out.rd = d;
    out.rn = n;
    out.rm = m;
    return true;
}
AluResult parallel_arithmetic(const Instruction& inst, std::uint32_t first,
    std::uint32_t second, std::uint32_t cpsr) noexcept
{
    const auto operation = static_cast<ParallelArithmeticOperation>(inst.opcode);
    if (operation == ParallelArithmeticOperation::Select) {
        std::uint32_t mask = 0;
        for (unsigned byte = 0; byte < 4; ++byte)
            if ((cpsr & (1U << (16U + byte))) != 0)
                mask |= 0xffU << (byte * 8U);
        return {(first & mask) | (second & ~mask), cpsr};
    }
    const auto layout = parallel_arithmetic_layout(operation);
    if (layout.exchange)
        second = std::rotl(second, 16);
    const auto mask = (1U << layout.bits) - 1U;
    const auto sign = 1U << (layout.bits - 1U);
    std::uint32_t result = 0, ge = 0;
    for (unsigned offset = 0; offset < 32; offset += layout.bits) {
        const auto lane = [&](std::uint32_t value) {
            const auto bits = (value >> offset) & mask;
            return inst.sign_extend
                ? static_cast<std::int32_t>(bits ^ sign) - static_cast<std::int32_t>(sign)
                : static_cast<std::int32_t>(bits);
        };
        const auto a = lane(first), b = lane(second);
        const bool subtract = layout.subtract(offset);
        const auto value = subtract ? a - b : a + b;
        const bool greater_equal = inst.sign_extend || subtract
            ? value >= 0 : static_cast<std::uint32_t>(value) > mask;
        result |= (static_cast<std::uint32_t>(value) & mask) << offset;
        if (greater_equal)
            ge |= (layout.bits == 16 ? 3U : 1U) << (offset / 8U);
    }
    return {result, (cpsr & ~0x000f0000U) | (ge << 16U)};
}
}
