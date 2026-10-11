/* SPDX-License-Identifier: MPL-2.0 */
#include "dual_transfer.hpp"

namespace ilemu::arm {
bool decode_dual_transfer(Instruction& out, std::uint32_t word, bool thumb,
    execution::ArmInstructionProfile architecture) noexcept
{
    if (thumb) {
        if ((word & 0xfe400000U) != 0xe8400000U ||
            (word & 0x01200000U) == 0)
            return false; // P=W=0 belongs to exclusive transfers.
        if (architecture == execution::ArmInstructionProfile::Armv6)
            return true;
        out.rn = (word >> 16U) & 15U;
        out.rd = (word >> 12U) & 15U;
        out.rs = (word >> 8U) & 15U;
        out.load = (word & (1U << 20U)) != 0;
        out.index = (word & (1U << 24U)) != 0;
        out.add = (word & (1U << 23U)) != 0;
        out.writeback = (word & (1U << 21U)) != 0;
        out.immediate_operand = true;
        out.immediate = (word & 255U) << 2U;
        if (out.rd == 13 || out.rd == 15 || out.rs == 13 || out.rs == 15 ||
            (out.load && out.rd == out.rs) || (!out.load && out.rn == 15))
            return true;
    } else {
        if (out.condition == 15 || (word & 0x0e1000d0U) != 0x000000d0U)
            return false;
        out.rn = (word >> 16U) & 15U;
        out.rd = (word >> 12U) & 15U;
        out.rs = out.rd + 1U;
        out.load = (word & 32U) == 0;
        out.index = (word & (1U << 24U)) != 0;
        out.add = (word & (1U << 23U)) != 0;
        out.writeback = !out.index || (word & (1U << 21U)) != 0;
        out.immediate_operand = (word & (1U << 22U)) != 0;
        if ((out.rd & 1U) != 0 || out.rs == 15 ||
            (!out.index && (word & (1U << 21U)) != 0))
            return true;
        if (out.immediate_operand) {
            out.immediate = ((word >> 4U) & 0xf0U) | (word & 15U);
        } else {
            out.rm = word & 15U;
            if ((word & 0xf00U) != 0 || out.rm == 15 ||
                (out.load && (out.rm == out.rd || out.rm == out.rs)))
                return true;
        }
    }
    if (out.writeback &&
        (out.rn == 15 || out.rn == out.rd || out.rn == out.rs))
        return true;
    out.align_pc = out.load && out.rn == 15 && out.immediate_operand;
    if (out.align_pc) {
        if (!out.index || out.writeback)
            return true;
    }
    out.access_size = 4;
    out.kind = InstructionKind::DualTransfer;
    return true;
}
}
