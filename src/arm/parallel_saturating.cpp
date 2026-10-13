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
    bool signed_values;
    if (thumb) {
        if ((word & 0xff80f0b0U) != 0xfa80f010U)
            return false;
        signed_values = (word & (1U << 6U)) == 0;
        constexpr int operations[] = { 4, 0, 1, -1, 5, 3, 2, -1 };
        operation = operations[(word >> 20U) & 7U];
        n = (word >> 16U) & 15U;
        d = (word >> 8U) & 15U;
        if (!execution::has_armv6t2_extensions(architecture) || n == 13 ||
            d == 13 || m == 13)
            return true;
    } else {
        if (out.condition == 15 || (word & 0x0fb00f10U) != 0x06200f10U)
            return false;
        signed_values = (word & (1U << 22U)) == 0;
        constexpr int operations[] = { 0, 1, 2, 3, 4, -1, -1, 5 };
        operation = operations[(word >> 5U) & 7U];
        n = (word >> 16U) & 15U;
        d = (word >> 12U) & 15U;
    }
    if (operation < 0 || n == 15 || d == 15 || m == 15)
        return true;
    out.kind = InstructionKind::ParallelSaturating;
    out.opcode = static_cast<unsigned>(operation) +
                 (signed_values ? static_cast<unsigned>(
                                      ParallelSaturatingOperation::SignedAdd16)
                                : 0U);
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
    const auto layout = parallel_saturating_layout(operation);
    const bool signed_values = operation >= Op::SignedAdd16;
    if (layout.exchange)
        second = std::rotl(second, 16);
    const auto bits = layout.bits;
    const auto mask = (1U << bits) - 1U;
    const auto sign = signed_values ? 1 << (bits - 1U) : 0;
    const auto minimum = -sign;
    const auto maximum = static_cast<std::int32_t>(mask) - sign;
    std::uint32_t result = 0;
    for (unsigned offset = 0; offset < 32; offset += bits) {
        const auto a =
            (static_cast<std::int32_t>((first >> offset) & mask) ^ sign) - sign;
        const auto b =
            (static_cast<std::int32_t>((second >> offset) & mask) ^ sign) -
            sign;
        const auto value = std::clamp(
            layout.subtract(offset) ? a - b : a + b, minimum, maximum);
        result |= (static_cast<std::uint32_t>(value) & mask) << offset;
    }
    return result;
}
}
