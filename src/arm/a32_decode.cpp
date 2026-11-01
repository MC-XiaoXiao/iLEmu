/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/a32_decode.hpp"
#include "simd_duplicate.hpp"
#include "simd_immediate_decode.hpp"
#include "simd_bitwise.hpp"
#include "simd_floating_decode.hpp"
#include "simd_integer_decode.hpp"
#include "simd_extract.hpp"
#include "simd_reverse.hpp"
#include "simd_conversion.hpp"
#include "simd_shift_immediate.hpp"
#include "simd_transfer.hpp"
#include "system_control.hpp"
#include "program_status_decode.hpp"
#include "exclusive.hpp"
#include "swap_decode.hpp"
#include "transform_decode.hpp"
#include "parallel_saturating_decode.hpp"
#include "parallel_arithmetic_decode.hpp"
#include "saturating_arithmetic_decode.hpp"
#include "core_vector_move.hpp"
#include "vfp_multiple.hpp"
#include "vfp_scalar.hpp"
#include "multiply_decode.hpp"
#include "integer_divide_decode.hpp"
#include "a32_transfer.hpp"
#include "dual_transfer.hpp"
#include "floating_decode.hpp"
#include "floating_arithmetic_decode.hpp"
#include "floating_comparison_decode.hpp"
#include "floating_immediate_decode.hpp"
#include "floating_system_decode.hpp"
#include <bit>

namespace ilemu::arm {
Instruction decode_a32(std::uint32_t word, execution::VfpRegisterProfile vfp,
    execution::ArmInstructionProfile architecture,
    execution::VfpInstructionProfile floating_instructions,
    execution::IntegerDivideProfile division,
    execution::AdvancedSimdProfile simd) noexcept
{
    Instruction out;
    out.condition = word >> 28;
    // The major encoding selects disjoint extension families. Each matcher
    // still owns its full mask, profile and operand validation; keep priority
    // within a class, including reserved encodings that terminate decoding.
    switch ((word >> 25U) & 7U) {
    case 0:
        if (decode_saturating_arithmetic(out, word, false, architecture) ||
            decode_multiply(out, word, false, architecture) ||
            decode_integer_transform(out, word, false, architecture) ||
            decode_swap(out, word) ||
            decode_dual_transfer(out, word, false, architecture) ||
            decode_exclusive(out, word, false) ||
            decode_program_status(out, word, false, architecture))
            return out;
        break;
    case 1:
        if (decode_program_status(out, word, false, architecture) ||
            decode_simd_immediate(out, word, false, vfp, architecture) ||
            decode_simd_floating(out, word, false, vfp, architecture, simd) ||
            decode_simd_integer(out, word, false, vfp, architecture, simd) ||
            decode_simd_duplicate(out, word, false) ||
            decode_simd_bitwise(out, word, false) ||
            decode_simd_extract(out, word, false) ||
            decode_simd_reverse(out, word, false, vfp, architecture, simd) ||
            decode_simd_shift_immediate(out, word, false, vfp, architecture, simd) ||
            decode_simd_conversion(out, word, false, vfp, architecture, simd))
            return out;
        break;
    case 2:
        if (decode_exclusive(out, word, false) ||
            decode_system_control(out, word, false) ||
            decode_simd_transfer(out, word, false))
            return out;
        break;
    case 3:
        if (decode_saturating_arithmetic(out, word, false, architecture) ||
            decode_multiply(out, word, false, architecture) ||
            decode_integer_divide(out, word, false, architecture, division) ||
            decode_integer_transform(out, word, false, architecture) ||
            decode_parallel_saturating(out, word, false, architecture) ||
            decode_parallel_arithmetic(out, word, false, architecture))
            return out;
        break;
    case 6:
    case 7:
        if (decode_floating_system(out, word, false, architecture) ||
            decode_floating_immediate(out, word, false, vfp, architecture, floating_instructions) ||
            decode_floating_comparison(out, word, false, vfp, architecture) ||
            decode_floating_arithmetic(out, word, false, vfp, architecture) ||
            decode_floating_conversion(out, word, false, vfp, architecture) ||
            decode_core_vector_move(out, word, false, vfp, architecture) ||
            decode_vfp_scalar(out, word, false, vfp, architecture) ||
            decode_vfp_multiple(out, word, false, vfp) ||
            decode_system_control(out, word, false) ||
            decode_simd_duplicate(out, word, false))
            return out;
        break;
    default:
        break;
    }
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
    if (out.condition == 15) {
        // ARM PLD is an optional data-cache hint from ARMv5TE onward.
        // It performs no architectural access, including for unmapped targets.
        const bool preload_immediate =
            (word & 0xff70f000U) == 0xf550f000U;
        const bool preload_register =
            (word & 0xff70f010U) == 0xf750f000U && (word & 15U) != 15U;
        if (preload_immediate || preload_register) {
            out.condition = 14;
            out.kind = InstructionKind::Nop;
        }
        return out;
    }
    // NOP and advisory YIELD are available from ARMv6K/ARMv6T2.
    // Other hints retain their separate architectural effects.
    if ((word & 0x0ffffffeU) == 0x0320f000U) {
        if (architecture != execution::ArmInstructionProfile::Armv6)
            out.kind = InstructionKind::Nop;
        return out;
    }
    // MOVW/MOVT share the existing Thumb wide-immediate execution path.
    if ((word & 0x0fb00000U) == 0x03000000U) {
        out.rd = (word >> 12U) & 15U;
        if (!execution::has_armv6t2_extensions(architecture) ||
            out.rd == 15)
            return out;
        out.kind = InstructionKind::WideImmediate;
        out.immediate = ((word >> 4U) & 0xf000U) | (word & 0xfffU);
        out.opcode = (word >> 22U) & 1U;
        return out;
    }
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
    if (decode_a32_transfer(out, word, architecture))
        return out;
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
    if (test ? (!out.set_flags || out.rd != 0) : (out.rd == 15 && out.set_flags))
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
            if (out.rd == 15 || out.rn == 15 || out.rm == 15 || out.rs == 15)
                return out;
        } else {
            out.shift_amount = (word >> 7) & 31U;
        }
    }
    out.kind = InstructionKind::DataProcessing;
    return out;
}
}
