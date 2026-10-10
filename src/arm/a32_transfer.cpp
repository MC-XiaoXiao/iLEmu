/* SPDX-License-Identifier: MPL-2.0 */
#include "a32_transfer.hpp"
namespace ilemu::arm {
bool decode_a32_transfer(Instruction& out, std::uint32_t word,
    execution::ArmInstructionProfile architecture) noexcept
{
    if (out.condition == 15)
        return false;
    const bool ordinary_transfer = (word & 0x0c000000U) == 0x04000000U;
    const bool extra_transfer =
        (word & 0x0e000090U) == 0x00000090U && (word & 0x60U) != 0;
    if (ordinary_transfer || extra_transfer) {
        out.rd = (word >> 12U) & 15U;
        out.rn = (word >> 16U) & 15U;
        out.load = (word & (1U << 20U)) != 0;
        out.index = (word & (1U << 24U)) != 0;
        out.add = (word & (1U << 23U)) != 0;
        out.writeback = !out.index || (word & (1U << 21U)) != 0;
        const bool unprivileged = !out.index && (word & (1U << 21U)) != 0;
        if (ordinary_transfer) {
            out.access_size = (word & (1U << 22U)) != 0 ? 1U : 4U;
            out.immediate_operand = (word & (1U << 25U)) == 0;
            if (out.immediate_operand)
                out.immediate = word & 0xfffU;
            else {
                if ((word & 16U) != 0)
                    return true; // media instruction encoding space
                out.rm = word & 15U;
                out.shift = static_cast<ShiftKind>((word >> 5U) & 3U);
                out.shift_amount = (word >> 7U) & 31U;
            }
        } else {
            const auto operation = (word >> 5U) & 3U;
            if (!out.load && operation != 1)
                return true; // LDRD/STRD have a different transfer contract.
            out.access_size = operation == 2 ? 1U : 2U;
            out.sign_extend = operation != 1;
            out.immediate_operand = (word & (1U << 22U)) != 0;
            if (out.immediate_operand)
                out.immediate = ((word >> 4U) & 0xf0U) | (word & 15U);
            else {
                if ((word & 0xf00U) != 0)
                    return true;
                out.rm = word & 15U;
            }
        }
        if ((!out.immediate_operand && out.rm == 15) ||
            (out.writeback && (out.rn == 15 || out.rn == out.rd)) ||
            (out.rd == 15 &&
                (out.access_size != 4 || (unprivileged && out.load))) ||
            (unprivileged && extra_transfer &&
                architecture == execution::ArmInstructionProfile::Armv6))
            return true;
        out.align_pc = out.load && out.rn == 15 && out.immediate_operand;
        out.kind = InstructionKind::Transfer;
        return true;
    }
    return false;
}
}
