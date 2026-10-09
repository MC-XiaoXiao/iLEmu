/* SPDX-License-Identifier: MPL-2.0 */
#include "t32_shifted.hpp"
#include "t32_data_opcode.hpp"

namespace ilemu::arm {
void decode_t32_shifted_register(
    Instruction& out, unsigned first, unsigned second) noexcept
{
    const auto op = (first >> 5U) & 15U;
    out.rn = first & 15U;
    out.rd = (second >> 8U) & 15U;
    out.rm = second & 15U;
    out.set_flags = (first & 16U) != 0;
    out.shift = static_cast<ShiftKind>((second >> 4U) & 3U);
    out.shift_amount = (((second >> 12U) & 7U) << 2U) |
                       ((second >> 6U) & 3U);
    if (op == 6) {
        if (out.set_flags || (second & 16U) != 0 ||
            out.rd == 13 || out.rd == 15 || out.rn == 13 ||
            out.rn == 15 || out.rm == 13 || out.rm == 15)
            return;
        out.kind = InstructionKind::PackHalfword;
        return;
    }
    const bool arithmetic = op == 8 || op == 13;
    const bool move = (op == 2 || op == 3) && out.rn == 15;
    const bool test = out.rd == 15 && out.set_flags &&
                      (op == 0 || op == 4 || arithmetic);
    const bool plain_move = op == 2 && move && !out.set_flags &&
                            out.shift == ShiftKind::Lsl &&
                            out.shift_amount == 0;
    if (plain_move) {
        if (out.rd == 15 || out.rm == 15 ||
            (out.rd == 13 && out.rm == 13))
            return;
    } else if ((out.rd == 15 && !test) ||
               (out.rd == 13 &&
                   !(arithmetic && out.rn == 13 &&
                       out.shift == ShiftKind::Lsl && out.shift_amount <= 3)) ||
               (out.rn == 13 && !arithmetic) ||
               (out.rn == 15 && !move) || out.rm == 13 || out.rm == 15)
        return;
    const auto opcode = t32_data_opcode(op, move, test);
    if (!opcode)
        return;
    out.opcode = *opcode;
    out.kind = InstructionKind::DataProcessing;
}
}
