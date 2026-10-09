/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/a32_decode.hpp"
#include "simd_duplicate.hpp"
#include "simd_bitwise.hpp"
#include "simd_transfer.hpp"
#include <bit>

namespace ilemu::arm {
Instruction decode_a32(std::uint32_t word) noexcept
{
    Instruction out;
    out.condition = word >> 28;
    if (decode_simd_duplicate(out, word, false) ||
        decode_simd_bitwise(out, word, false) ||
        decode_simd_transfer(out, word, false))
        return out;
    // BLX immediate occupies the unconditional branch encoding space.
    if ((word & 0xfe000000U) == 0xfa000000U) {
        out.condition = 14;
        out.kind = InstructionKind::Branch;
        out.link = out.exchange = true;
        out.immediate = ((word & 0xffffffU) << 2U) | ((word >> 23U) & 2U);
        if ((out.immediate & (1U << 25U)) != 0)
            out.immediate |= 0xfc000000U;
        return out;
    }
    // Unconditional encoding space has different decode rules.
    if (out.condition == 15)
        return out;
    if ((word & 0xfff000f0U) == 0xe1200070U) {
        out.kind = InstructionKind::Breakpoint;
        out.immediate = ((word >> 4U) & 0xfff0U) | (word & 15U);
        return out;
    }
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
    if ((word & 0x0e000000U) == 0x08000000U) {
        out.rn = (word >> 16U) & 15U;
        out.registers = word & 0xffffU;
        out.load = (word & (1U << 20U)) != 0;
        out.writeback = (word & (1U << 21U)) != 0;
        out.add = (word & (1U << 23U)) != 0;
        out.index = (word & (1U << 24U)) != 0;
        if ((word & (1U << 22U)) != 0 || out.rn == 15 || out.registers == 0 ||
            (out.writeback && (out.registers & (1U << out.rn)) != 0 &&
                (out.load || static_cast<unsigned>(
                                 std::countr_zero(out.registers)) != out.rn)))
            return out;
        out.kind = InstructionKind::MultipleTransfer;
        return out;
    }
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
                    return out; // media instruction encoding space
                out.rm = word & 15U;
                out.shift = static_cast<ShiftKind>((word >> 5U) & 3U);
                out.shift_amount = (word >> 7U) & 31U;
            }
        } else {
            const auto operation = (word >> 5U) & 3U;
            if (!out.load && operation != 1)
                return out; // LDRD/STRD have a different transfer contract.
            out.access_size = operation == 2 ? 1U : 2U;
            out.sign_extend = operation != 1;
            out.immediate_operand = (word & (1U << 22U)) != 0;
            if (out.immediate_operand)
                out.immediate = ((word >> 4U) & 0xf0U) | (word & 15U);
            else {
                if ((word & 0xf00U) != 0)
                    return out;
                out.rm = word & 15U;
            }
        }
        if ((!out.immediate_operand && out.rm == 15) ||
            (out.writeback && (out.rn == 15 || out.rn == out.rd)) ||
            (out.rd == 15 && (out.access_size != 4 || unprivileged)) ||
            (out.rn == 15 &&
                (!out.immediate_operand || !out.index || unprivileged)))
            return out;
        out.kind = InstructionKind::Transfer;
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
