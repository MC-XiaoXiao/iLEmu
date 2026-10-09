/* SPDX-License-Identifier: MPL-2.0 */
#include "t32_transfer.hpp"

namespace ilemu::arm {
void decode_t32_transfer(Instruction& out, unsigned first, unsigned second,
    bool last_in_it) noexcept
{
    const auto size = (first >> 5U) & 3U;
    const bool signed_load = (first & 0x100U) != 0;
    out.load = (first & 16U) != 0;
    if (size == 3 || (signed_load && (!out.load || size == 2)))
        return;
    out.access_size = 1U << size;
    out.sign_extend = signed_load;
    out.rn = first & 15U;
    out.rd = second >> 12U;
    const bool literal = out.load && out.rn == 15;
    const bool imm12 = (first & 0x80U) != 0;
    const bool register_offset = !literal && !imm12 &&
                                 (second & 0x0fc0U) == 0;
    if (out.rn == 15 && !literal)
        return;
    if (literal || imm12) {
        out.immediate_operand = true;
        out.immediate = second & 0xfffU;
        out.add = imm12;
        out.align_pc = literal;
    } else if (register_offset) {
        out.rm = second & 15U;
        out.shift_amount = (second >> 4U) & 3U;
    } else {
        if ((second & 0x0800U) == 0)
            return;
        out.index = (second & 0x0400U) != 0;
        out.add = (second & 0x0200U) != 0;
        out.writeback = (second & 0x0100U) != 0;
        // P=1,U=1,W=0 is the separate unprivileged-access family.
        if ((!out.index && !out.writeback) ||
            (out.index && out.add && !out.writeback))
            return;
        out.immediate_operand = true;
        out.immediate = second & 255U;
    }
    if (out.load && size != 2 && out.rd == 15) {
        if (out.writeback)
            return;
        // PLD/PLDW/PLI and unallocated memory hints have no required effect.
        if (register_offset && !(signed_load && size == 1) &&
            (out.rm == 13 || out.rm == 15))
            return;
        out.kind = InstructionKind::Nop;
        return;
    }
    if ((!out.load && out.rd == 15) || (size != 2 && out.rd == 13) ||
        (register_offset && (out.rm == 13 || out.rm == 15)) ||
        (out.writeback && out.rn == out.rd) ||
        (out.load && out.rd == 15 && !last_in_it))
        return;
    out.kind = InstructionKind::Transfer;
}
}
