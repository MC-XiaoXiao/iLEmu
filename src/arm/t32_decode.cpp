/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/t32_decode.hpp"
#include "t32_immediate.hpp"
#include "t32_shifted.hpp"
#include <bit>

namespace ilemu::arm {
namespace {
    std::uint32_t sign_extend(std::uint32_t value, unsigned bits)
    {
        const auto sign = 1U << (bits - 1U);
        return (value ^ sign) - sign;
    }
}
Instruction decode_t32(std::uint16_t first, std::optional<std::uint16_t> second,
    unsigned it) noexcept
{
    Instruction out;
    out.size = thumb_is_wide(first) ? 4U : 2U;
    out.pc_offset = 4;
    out.condition = it == 0 ? 14U : it >> 4U;
    const bool last = it == 0 || (it & 7U) == 0;
    const unsigned word = first;
    const auto alu = [&](unsigned op, bool flags) {
        out.kind = InstructionKind::DataProcessing;
        out.opcode = op;
        out.set_flags = flags;
    };
    const auto transfer = [&](unsigned size, bool load) {
        out.kind = InstructionKind::Transfer;
        out.access_size = size;
        out.load = load;
    };
    if (out.size == 4) {
        if (!second)
            return out;
        const unsigned tail = *second;
        if ((word & 0xfe00U) == 0xea00U && (tail & 0x8000U) == 0) {
            decode_t32_shifted_register(out, word, tail);
        } else if ((word & 0xf800U) == 0xf000U && (tail & 0x8000U) != 0) {
            const auto s = (word >> 10U) & 1U;
            const auto j1 = (tail >> 13U) & 1U, j2 = (tail >> 11U) & 1U;
            if ((tail & 0xd000U) == 0x8000U) { // B<c>.W
                const auto cond = (word >> 6U) & 15U;
                if (cond >= 14 || it != 0)
                    return out;
                out.condition = cond;
                out.immediate = sign_extend(
                    (s << 20U) | (j2 << 19U) | (j1 << 18U) |
                        ((word & 63U) << 12U) | ((tail & 0x7ffU) << 1U),
                    21);
            } else {
                if (!last)
                    return out;
                out.link = (tail & 0x4000U) != 0;
                out.exchange = (tail & 0x1000U) == 0;
                if (out.exchange && (!out.link || (tail & 1U) != 0))
                    return out;
                const auto i1 = (j1 ^ s) ^ 1U, i2 = (j2 ^ s) ^ 1U;
                out.immediate = sign_extend(
                    (s << 24U) | (i1 << 23U) | (i2 << 22U) |
                        ((word & 0x3ffU) << 12U) | ((tail & 0x7ffU) << 1U),
                    25);
                out.align_pc = out.exchange;
            }
            out.kind = InstructionKind::Branch;
        } else if ((word & 0xfa00U) == 0xf000U &&
                   (tail & 0x8000U) == 0) {
            decode_t32_modified_immediate(out, word, tail);
        } else if (((word & 0xfbf0U) == 0xf240U ||
                       (word & 0xfbf0U) == 0xf2c0U) &&
                   (tail & 0x8000U) == 0) { // MOVW / MOVT
            out.rd = (tail >> 8U) & 15U;
            if (out.rd == 13 || out.rd == 15)
                return out;
            out.kind = InstructionKind::WideImmediate;
            out.immediate = ((word & 15U) << 12U) |
                            (((word >> 10U) & 1U) << 11U) |
                            (((tail >> 12U) & 7U) << 8U) | (tail & 255U);
            out.opcode = (word >> 7U) & 1U;
        }
        if ((word & 0xffd0U) == 0xe880U || (word & 0xffd0U) == 0xe890U ||
            (word & 0xffd0U) == 0xe900U || (word & 0xffd0U) == 0xe910U) {
            out.rn = word & 15U;
            out.registers = tail;
            out.load = (word & 16U) != 0;
            out.writeback = (word & 32U) != 0;
            out.add = (word & 0x100U) == 0;
            out.index = !out.add;
            if (out.rn == 15 || std::popcount(out.registers) < 2 ||
                (out.registers & (out.load ? 0x2000U : 0xa000U)) != 0 ||
                (out.load && (out.registers & 0xc000U) == 0xc000U) ||
                (out.writeback && (out.registers & (1U << out.rn)) != 0) ||
                (out.load && (out.registers & 0x8000U) != 0 && !last))
                return out;
            out.kind = InstructionKind::MultipleTransfer;
        }
        return out;
    }
    if ((word & 0xe000U) == 0 && (word & 0x1800U) != 0x1800U) {
        if (it != 0 && (word & 0xffc0U) == 0)
            return out; // MOVS T2 is not permitted inside IT.
        alu(13, it == 0); // immediate shifts
        out.rd = word & 7U;
        out.rm = (word >> 3U) & 7U;
        out.shift = static_cast<ShiftKind>((word >> 11U) & 3U);
        out.shift_amount = (word >> 6U) & 31U;
    } else if ((word & 0xf800U) == 0x1800U) {
        alu((word & 0x200U) != 0 ? 2U : 4U, it == 0);
        out.rd = word & 7U;
        out.rn = (word >> 3U) & 7U;
        out.immediate_operand = (word & 0x400U) != 0;
        out.rm = out.immediate = (word >> 6U) & 7U;
    } else if ((word & 0xe000U) == 0x2000U) {
        constexpr unsigned ops[] = { 13, 10, 4, 2 };
        const auto op = (word >> 11U) & 3U;
        alu(ops[op], op == 1 || it == 0);
        out.rd = (word >> 8U) & 7U;
        out.rn = op == 0 ? 0U : out.rd;
        out.immediate_operand = true;
        out.immediate = word & 255U;
    } else if ((word & 0xfc00U) == 0x4000U) {
        constexpr unsigned ops[] = { 0, 1, 13, 13, 13, 5, 6, 13, 8, 3, 10, 11,
            12, 0, 14, 15 };
        const auto op = (word >> 6U) & 15U;
        alu(ops[op], it == 0 || op == 8 || op == 10 || op == 11);
        out.rd = out.rn = word & 7U;
        out.rm = (word >> 3U) & 7U;
        if (op == 2 || op == 3 || op == 4 || op == 7) {
            out.register_shift = true;
            out.rs = out.rm;
            out.rm = out.rd;
            out.shift =
                op == 7 ? ShiftKind::Ror : static_cast<ShiftKind>(op - 2U);
        } else if (op == 9) {
            out.rn = out.rm;
            out.immediate_operand = true;
        } else if (op == 13) {
            out.kind = InstructionKind::Multiply;
            out.rs = out.rm;
            out.rm = out.rd;
        }
    } else if ((word & 0xfc00U) == 0x4400U) {
        const auto op = (word >> 8U) & 3U;
        out.rd = out.rn = (word & 7U) | ((word >> 4U) & 8U);
        out.rm = (word >> 3U) & 15U;
        if (op == 3) {
            out.link = (word & 0x80U) != 0;
            if ((word & 7U) != 0 || !last || (out.link && out.rm == 15))
                return out;
            out.kind = InstructionKind::BranchExchange;
        } else {
            if ((op == 1 && (out.rn == 15 || out.rm == 15 ||
                                (out.rn < 8 && out.rm < 8))) ||
                (op == 0 && out.rn == 15 && out.rm == 15) ||
                (op != 1 && out.rd == 15 && !last))
                return out;
            alu(op == 0 ? 4U : op == 1 ? 10U : 13U, op == 1);
        }
    } else if ((word & 0xf800U) == 0x4800U) {
        transfer(4, true);
        out.rd = (word >> 8U) & 7U;
        out.rn = 15;
        out.align_pc = out.immediate_operand = true;
        out.immediate = (word & 255U) << 2U;
    } else if ((word & 0xf000U) == 0x5000U) {
        const auto op = (word >> 9U) & 7U;
        constexpr unsigned sizes[] = { 4, 2, 1, 1, 4, 2, 1, 2 };
        transfer(sizes[op], op >= 3);
        out.sign_extend = op == 3 || op == 7;
        out.rd = word & 7U;
        out.rn = (word >> 3U) & 7U;
        out.rm = (word >> 6U) & 7U;
    } else if ((word & 0xe000U) == 0x6000U || (word & 0xf000U) == 0x8000U) {
        const auto size = (word & 0xf000U) == 0x8000U ? 2U
                          : (word & 0x1000U) != 0     ? 1U
                                                      : 4U;
        transfer(size, (word & 0x800U) != 0);
        out.rd = word & 7U;
        out.rn = (word >> 3U) & 7U;
        out.immediate_operand = true;
        out.immediate = ((word >> 6U) & 31U) * size;
    } else if ((word & 0xf000U) == 0x9000U) {
        transfer(4, (word & 0x800U) != 0);
        out.rd = (word >> 8U) & 7U;
        out.rn = 13;
        out.immediate_operand = true;
        out.immediate = (word & 255U) << 2U;
    } else if ((word & 0xf000U) == 0xa000U || (word & 0xff00U) == 0xb000U) {
        const bool adjust_sp = (word & 0xf000U) == 0xb000U;
        alu(adjust_sp && (word & 0x80U) != 0 ? 2U : 4U, false);
        out.rd = adjust_sp ? 13U : (word >> 8U) & 7U;
        out.rn = adjust_sp || (word & 0x800U) != 0 ? 13U : 15U;
        out.align_pc = out.rn == 15;
        out.immediate_operand = true;
        out.immediate = (word & (adjust_sp ? 127U : 255U)) << 2U;
    } else if ((word & 0xf600U) == 0xb400U) { // PUSH / POP
        out.rn = 13;
        out.load = (word & 0x800U) != 0;
        out.registers =
            (word & 255U) |
            ((word & 0x100U) != 0 ? (out.load ? 0x8000U : 0x4000U) : 0U);
        out.writeback = true;
        out.add = out.load;
        out.index = !out.load;
        if (out.registers != 0 &&
            (!out.load || (out.registers & 0x8000U) == 0 || last))
            out.kind = InstructionKind::MultipleTransfer;
    } else if ((word & 0xf000U) == 0xc000U) { // STM / LDM T1
        out.rn = (word >> 8U) & 7U;
        out.load = (word & 0x800U) != 0;
        out.registers = word & 255U;
        out.add = true;
        out.index = false;
        out.writeback = !out.load || (out.registers & (1U << out.rn)) == 0;
        if (out.registers != 0 &&
            (out.load || (out.registers & (1U << out.rn)) == 0 ||
                static_cast<unsigned>(std::countr_zero(out.registers)) ==
                    out.rn))
            out.kind = InstructionKind::MultipleTransfer;
    } else if ((word & 0xf500U) == 0xb100U && it == 0) {
        out.kind = InstructionKind::CompareBranch;
        out.rn = word & 7U;
        out.opcode = (word >> 11U) & 1U;
        out.immediate = ((word & 0xf8U) >> 2U) | ((word & 0x200U) >> 3U);
    } else if ((word & 0xff00U) == 0xbe00U && it == 0) {
        out.kind = InstructionKind::Breakpoint;
        out.immediate = word & 255U;
    } else if ((word & 0xff00U) == 0xbf00U) {
        const auto mask = word & 15U, cond = (word >> 4U) & 15U;
        if (mask == 0) {
            if (cond == 0)
                out.kind = InstructionKind::Nop;
        } else if (it == 0 && cond != 15 &&
                   (cond != 14 || std::popcount(mask) == 1)) {
            out.kind = InstructionKind::IfThen;
            out.immediate = word & 255U;
        }
    } else if ((word & 0xf000U) == 0xd000U) {
        const auto cond = (word >> 8U) & 15U;
        if (cond < 14 && it == 0) {
            out.kind = InstructionKind::Branch;
            out.condition = cond;
            out.immediate = sign_extend((word & 255U) << 1U, 9);
        } else if (cond == 15) {
            out.kind = InstructionKind::Svc;
            out.immediate = word & 255U;
        }
    } else if ((word & 0xf800U) == 0xe000U && last) {
        out.kind = InstructionKind::Branch;
        out.immediate = sign_extend((word & 0x7ffU) << 1U, 12);
    }
    return out;
}
}
