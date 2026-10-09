/* SPDX-License-Identifier: MPL-2.0 */
#include "t32_immediate.hpp"
#include <bit>

namespace ilemu::arm {
void decode_t32_modified_immediate(
    Instruction& out, unsigned first, unsigned second) noexcept
{
    const auto op = (first >> 5U) & 15U;
    out.rn = first & 15U;
    out.rd = (second >> 8U) & 15U;
    out.set_flags = (first & 16U) != 0;
    const bool arithmetic = op == 8 || op == 13;
    const bool move = (op == 2 || op == 3) && out.rn == 15;
    const bool test = out.rd == 15 && out.set_flags &&
                      (op == 0 || op == 4 || arithmetic);
    if ((out.rd == 15 && !test) ||
        (out.rd == 13 && !(arithmetic && out.rn == 13)) ||
        (out.rn == 13 && !arithmetic) || (out.rn == 15 && !move))
        return;
    switch (op) {
    case 0: out.opcode = test ? 8U : 0U; break;
    case 1: out.opcode = 14; break;
    case 2: out.opcode = move ? 13U : 12U; break;
    case 3: out.opcode = move ? 15U : opcode_orn; break;
    case 4: out.opcode = test ? 9U : 1U; break;
    case 8: out.opcode = test ? 11U : 4U; break;
    case 10: out.opcode = 5; break;
    case 11: out.opcode = 6; break;
    case 13: out.opcode = test ? 10U : 2U; break;
    case 14: out.opcode = 3; break;
    default: return;
    }
    const auto imm12 = (((first >> 10U) & 1U) << 11U) |
                       (((second >> 12U) & 7U) << 8U) | (second & 255U);
    if ((imm12 & 0xc00U) == 0) {
        const std::uint32_t byte = imm12 & 255U;
        const auto pattern = imm12 >> 8U;
        if (pattern != 0 && byte == 0)
            return; // Zero replication encodings are unpredictable.
        switch (pattern) {
        case 0: out.immediate = byte; break;
        case 1: out.immediate = byte | (byte << 16U); break;
        case 2: out.immediate = (byte << 8U) | (byte << 24U); break;
        case 3: out.immediate = byte * 0x01010101U; break;
        }
    } else {
        out.shift_amount = imm12 >> 7U;
        out.immediate = std::rotr(
            std::uint32_t { 0x80U | (imm12 & 127U) },
            static_cast<int>(out.shift_amount));
    }
    out.immediate_operand = true;
    out.kind = InstructionKind::DataProcessing;
}
}
