/* SPDX-License-Identifier: MPL-2.0 */
#include "t32_plain_immediate.hpp"
namespace ilemu::arm {
void decode_t32_plain_arithmetic(Instruction& out, unsigned first,
    unsigned second, execution::ThumbArithmeticProfile profile) noexcept
{
    out.rn = first & 15U;
    out.rd = (second >> 8U) & 15U;
    if (out.rd == 15 ||
        (out.rd == 13 && out.rn != 13 &&
            profile !=
                execution::ThumbArithmeticProfile::GeneralPurposeStackPointer))
        return;
    out.opcode = (first & 0x80U) != 0 ? 2U : 4U;
    out.immediate = (((first >> 10U) & 1U) << 11U) |
                    (((second >> 12U) & 7U) << 8U) | (second & 255U);
    out.immediate_operand = true;
    out.align_pc = out.rn == 15; // ADR uses Align(PC + 4, 4).
    out.kind = InstructionKind::DataProcessing;
}
}
