/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/a32_decode.hpp"
#include <bit>

namespace ilemu::arm {
A32Instruction decode_a32(std::uint32_t word) noexcept
{
    A32Instruction out;
    out.condition = word >> 28;
    // Unconditional encoding space has different decode rules.
    if (out.condition == 15)
        return out;
    if ((word & 0x0f000000U) == 0x0f000000U) {
        out.kind = InstructionKind::Svc;
        out.immediate = word & 0xffffffU;
        return out;
    }
    if ((word & 0x0e000000U) == 0x0a000000U) {
        out.kind = InstructionKind::Branch;
        out.link = (word & (1U << 24)) != 0;
        out.immediate = (word & 0xffffffU) << 2;
        if ((out.immediate & (1U << 25)) != 0)
            out.immediate |= 0xfc000000U;
        return out;
    }
    if ((word & 0x0ffffff0U) == 0x012fff10U ||
        (word & 0x0ffffff0U) == 0x012fff30U) {
        out.rm = word & 15U;
        out.link = (word & 0x20U) != 0;
        if (out.link && out.rm == 15)
            return out;
        out.kind = InstructionKind::BranchExchange;
        return out;
    }
    if ((word & 0x0fc000f0U) == 0x00000090U) {
        out.rd = (word >> 16) & 15U;
        out.rn = (word >> 12) & 15U;
        out.rs = (word >> 8) & 15U;
        out.rm = word & 15U;
        out.accumulate = (word & (1U << 21)) != 0;
        out.set_flags = (word & (1U << 20)) != 0;
        if (out.rd == 15 || out.rs == 15 || out.rm == 15 ||
            (out.accumulate ? out.rn == 15 : out.rn != 0))
            return out;
        out.kind = InstructionKind::Multiply;
        return out;
    }
    const bool immediate = (word & 0x0e000000U) == 0x02000000U;
    const bool shifted_register =
        (word & 0x0e000000U) == 0 && (word & 0x90U) != 0x90U;
    if (!immediate && !shifted_register)
        return out;
    out.opcode = (word >> 21) & 15U;
    out.rd = (word >> 12) & 15U;
    out.rn = (word >> 16) & 15U;
    out.set_flags = (word & (1U << 20)) != 0;
    const bool test = out.opcode >= 8 && out.opcode <= 11;
    if (test ? (!out.set_flags || out.rd != 0) : out.rd == 15)
        return out;
    if ((out.opcode == 13 || out.opcode == 15) && out.rn != 0)
        return out;
    out.immediate_operand = immediate;
    if (immediate) {
        out.shift_amount = ((word >> 8) & 15U) * 2;
        out.immediate =
            std::rotr(word & 255U, static_cast<int>(out.shift_amount));
    } else {
        out.rm = word & 15U;
        out.shift = static_cast<ShiftKind>((word >> 5) & 3U);
        out.register_shift = (word & 16U) != 0;
        if (out.register_shift) {
            out.rs = (word >> 8) & 15U;
            if (out.rn == 15 || out.rm == 15 || out.rs == 15)
                return out;
        } else {
            out.shift_amount = (word >> 7) & 31U;
        }
    }
    out.kind = InstructionKind::DataProcessing;
    return out;
}
}
