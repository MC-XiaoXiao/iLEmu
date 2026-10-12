/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/parallel_saturating.hpp"
#include "parallel_saturating_decode.hpp"
#include <algorithm>
#include <bit>
namespace ilemu::arm {
bool decode_parallel_saturating(Instruction& out, std::uint32_t word,
    bool thumb, execution::ArmInstructionProfile architecture) noexcept
{
    unsigned n, d, m = word & 15U;
    int operation;
    if (thumb) {
        if ((word & 0xff80f0f0U) != 0xfa80f050U)
            return false;
        constexpr int operations[] = { 4, 0, 1, -1, 5, 3, 2, -1 };
        operation = operations[(word >> 20U) & 7U];
        n = (word >> 16U) & 15U;
        d = (word >> 8U) & 15U;
        if (!execution::has_armv6t2_extensions(architecture) ||
            n == 13 || d == 13 || m == 13)
            return true;
    } else {
        if (out.condition == 15 || (word & 0x0ff00f10U) != 0x06600f10U)
            return false;
        constexpr int operations[] = { 0, 1, 2, 3, 4, -1, -1, 5 };
        operation = operations[(word >> 5U) & 7U];
        n = (word >> 16U) & 15U;
        d = (word >> 12U) & 15U;
    }
    if (operation < 0 || n == 15 || d == 15 || m == 15)
        return true;
    out.kind = InstructionKind::ParallelSaturating;
    out.opcode = static_cast<unsigned>(operation);
    out.rd = d;
    out.rn = n;
    out.rm = m;
    return true;
}
std::uint32_t parallel_saturating(
    const Instruction& inst, std::uint32_t first, std::uint32_t second) noexcept
{
    using Op = ParallelSaturatingOperation;
    const auto operation = static_cast<Op>(inst.opcode);
    const bool exchange = operation == Op::UnsignedAddSubtract ||
                          operation == Op::UnsignedSubtractAdd;
    if (exchange)
        second = std::rotl(second, 16);
    const unsigned bits =
        operation == Op::UnsignedAdd8 || operation == Op::UnsignedSubtract8
            ? 8U
            : 16U;
    const auto mask = (1U << bits) - 1U;
    std::uint32_t result = 0;
    for (unsigned offset = 0; offset < 32; offset += bits) {
        const auto a = (first >> offset) & mask, b = (second >> offset) & mask;
        const bool subtract =
            operation == Op::UnsignedSubtract8 ||
            operation == Op::UnsignedSubtract16 ||
            (operation == Op::UnsignedAddSubtract && offset == 0) ||
            (operation == Op::UnsignedSubtractAdd && offset == 16);
        const auto value =
            subtract ? (a >= b ? a - b : 0U) : std::min(a + b, mask);
        result |= value << offset;
    }
    return result;
}
}
