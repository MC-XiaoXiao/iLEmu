/* SPDX-License-Identifier: MPL-2.0 */
#include "integer_emitter.hpp"
#include "arm/multiply.hpp"
#include "arm/pc_write.hpp"
#include "arm/saturating_arithmetic.hpp"
namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
bool IntegerEmitter::clobbers_internal_flags(const arm::Instruction& inst) noexcept
{
    if (inst.kind == arm::InstructionKind::SaturatingArithmetic) {
        using Op = arm::SaturatingOperation;
        switch (static_cast<Op>(inst.opcode)) {
        case Op::Signed: return inst.immediate != 32;
        case Op::SignedHalfwords: return inst.immediate != 16;
        case Op::Unsigned: return inst.immediate < 31;
        case Op::UnsignedHalfwords: return inst.immediate < 15;
        default: return true;
        }
    }
    if (inst.kind == arm::InstructionKind::Multiply) {
        using Op = arm::MultiplyOperation;
        switch (static_cast<Op>(inst.opcode)) {
        case Op::Halfword:
        case Op::WordHalfword:
        case Op::DualHalfwordSubtract: return inst.accumulate;
        case Op::DualHalfword: return true;
        default: break;
        }
    }
    return false;
}
bool IntegerEmitter::preserves_vector_scratch(const arm::Instruction& inst) noexcept
{
    using Kind = arm::InstructionKind;
    switch (inst.kind) {
    case Kind::DataProcessing:
        return !arm::alu_writes_pc(inst);
    case Kind::Multiply:
        return !arm::dual_halfword_multiply(
            static_cast<arm::MultiplyOperation>(inst.opcode));
    case Kind::SaturatingArithmetic: {
        const auto operation = static_cast<arm::SaturatingOperation>(inst.opcode);
        return operation != arm::SaturatingOperation::SignedHalfwords &&
               operation != arm::SaturatingOperation::UnsignedHalfwords;
    }
    case Kind::WideImmediate:
    case Kind::IntegerDivide:
    case Kind::IntegerTransform:
    case Kind::PackHalfword:
    case Kind::ParallelArithmetic:
    case Kind::IfThen:
    case Kind::Nop:
        return true;
    default:
        return false;
    }
}
void IntegerEmitter::divide(const arm::Instruction& inst)
{
    const WReg d { static_cast<int>(inst.rd) }, n { static_cast<int>(inst.rn) },
        m { static_cast<int>(inst.rm) };
    if (inst.sign_extend)
        code_.SDIV(d, n, m);
    else
        code_.UDIV(d, n, m);
}
void IntegerEmitter::merge_nz()
{
    code_.MRS(X17, SystemReg::NZCV);
    // Keep the new N/Z and restore the shifter snapshot C/V. MRS NZCV
    // supplies zero below bit28, so one bitfield insert replaces three ALU ops.
    code_.BFI(W17, W21, 0, 30);
    code_.MSR(SystemReg::NZCV, X17);
}
WReg IntegerEmitter::reg(unsigned index, std::uint32_t pc, WReg scratch)
{
    if (index != 15)
        return WReg { static_cast<int>(index) };
    code_.MOV(scratch, pc);
    return scratch;
}
WReg IntegerEmitter::lhs(const arm::Instruction& inst, std::uint32_t pc)
{
    // MOV/MVN have no first source. In Thumb aliases Rn encodes PC, but
    // materializing that value would only add a dead host instruction.
    return inst.opcode == 13 || inst.opcode == 15 ? WZR : reg(inst.rn, pc, W15);
}
void IntegerEmitter::carry_from(
    WReg value, unsigned bit, LogicalFlagOutput flags)
{
    if (flags == LogicalFlagOutput::CarryOnly) {
        if (bit == 31)
            code_.ADDS(WZR, value, value);
        else {
            code_.UBFX(W15, value, bit, 1);
            code_.CMP(W15, 1);
        }
        return;
    }
    if (bit != 0)
        code_.LSR(W15, value, bit);
    else
        code_.MOV(W15, value);
    code_.BFI(X21, X15, 29, 1);
}
void IntegerEmitter::shifter(
    const arm::Instruction& inst, std::uint32_t pc, bool carry_output,
    LogicalFlagOutput flags)
{
    if (carry_output && flags == LogicalFlagOutput::All)
        code_.MRS(X21, SystemReg::NZCV);
    if (inst.immediate_operand) {
        code_.MOV(W16, inst.immediate);
        if (carry_output && inst.shift_amount != 0)
            carry_from(W16, 31, flags);
        return;
    }
    // Register-shift forms already reject architectural PC operands.
    const auto rm = reg(inst.rm, pc, W16);
    const auto amount = inst.shift_amount;
    if (!inst.register_shift) {
        switch (inst.shift) {
        case arm::ShiftKind::Lsl:
            if (amount == 0)
                code_.MOV(W16, rm);
            else {
                if (carry_output)
                    carry_from(rm, 32 - amount, flags);
                code_.LSL(W16, rm, amount);
            }
            break;
        case arm::ShiftKind::Lsr:
            if (carry_output)
                carry_from(rm, amount == 0 ? 31 : amount - 1, flags);
            if (amount == 0)
                code_.MOV(W16, 0);
            else
                code_.LSR(W16, rm, amount);
            break;
        case arm::ShiftKind::Asr:
            if (carry_output)
                carry_from(rm, amount == 0 ? 31 : amount - 1, flags);
            code_.ASR(W16, rm, amount == 0 ? 31 : amount);
            break;
        case arm::ShiftKind::Ror:
            if (amount == 0) {
                if (flags == LogicalFlagOutput::CarryOnly)
                    code_.CSET(W17, Cond::CS);
                else {
                    if (!carry_output)
                        code_.MRS(X21, SystemReg::NZCV);
                    code_.LSR(W17, W21, 29);
                } // Preserve incoming C before replacing it.
                if (carry_output)
                    carry_from(rm, 0, flags);
                code_.LSR(W16, rm, 1);
                code_.BFI(W16, W17, 31, 1);
            } else {
                if (carry_output)
                    carry_from(rm, amount - 1, flags);
                code_.ROR(W16, rm, amount);
            }
            break;
        }
        return;
    }
    if (shift_range_ && shift_range_->precise() && shift_range_->accepts(inst)) {
        bounded_shifter(inst, W16, rm, carry_output, flags);
        return;
    }
    if (inst.shift == arm::ShiftKind::Ror && !carry_output) {
        // Register ROR uses the low byte, then modulo 32. A64 RORV's low
        // five-bit count is equivalent, including zero, without flag changes.
        code_.RORV(W16, rm, WReg { static_cast<int>(inst.rs) });
        return;
    }
    if (!carry_output && shift_range_ && shift_range_->accepts(inst)) {
        bounded_shift(inst, W16, rm);
        return;
    }
    Label zero, large, done, carry_zero;
    code_.AND(W17, WReg { static_cast<int>(inst.rs) }, 255);
    code_.CBZ(W17, zero);
    if (inst.shift == arm::ShiftKind::Ror) {
        code_.RORV(W16, rm, W17);
        if (carry_output)
            carry_from(W16, 31, flags);
    } else {
        code_.LSR(W15, W17, 5);
        code_.CBNZ(W15, large);
        if (carry_output) {
            if (flags == LogicalFlagOutput::CarryOnly) {
                // For counts 1..31, move the guest carry bit to bit 63.
                // Doubling publishes that bit as native C; N/Z/V are dead.
                if (inst.shift == arm::ShiftKind::Lsl)
                    code_.ADD(W15, W17, 31);
                else
                    code_.NEG(W15, W17);
                code_.LSLV(X15, rm.toX(), X15);
                code_.ADDS(XZR, X15, X15);
            } else {
                if (inst.shift == arm::ShiftKind::Lsl)
                    code_.NEG(W15, W17);
                else
                    code_.SUB(W15, W17, 1);
                code_.LSRV(W15, rm, W15);
                code_.BFI(X21, X15, 29, 1);
            }
        }
        switch (inst.shift) {
        case arm::ShiftKind::Lsl:
            code_.LSLV(W16, rm, W17);
            break;
        case arm::ShiftKind::Lsr:
            code_.LSRV(W16, rm, W17);
            break;
        case arm::ShiftKind::Asr:
            code_.ASRV(W16, rm, W17);
            break;
        default:
            break;
        }
        code_.B(done);
        code_.l(large);
        if (inst.shift == arm::ShiftKind::Asr) {
            code_.ASR(W16, rm, 31);
            if (carry_output)
                carry_from(rm, 31, flags);
        } else {
            code_.MOV(W16, 0);
            if (carry_output) {
                code_.EOR(W15, W17, 32);
                code_.CBNZ(W15, carry_zero);
                carry_from(rm, inst.shift == arm::ShiftKind::Lsl ? 0 : 31, flags);
                code_.B(done);
                code_.l(carry_zero);
                if (flags == LogicalFlagOutput::CarryOnly)
                    code_.CMN(WZR, WZR, AddSubShift::LSL, 0);
                else
                    code_.BFI(X21, XZR, 29, 1);
            }
        }
    }
    code_.B(done);
    code_.l(zero);
    code_.MOV(W16, rm);
    code_.l(done);
}
bool IntegerEmitter::shifted_alu(const arm::Instruction& inst, std::uint32_t pc)
{
    if (inst.register_shift && inst.opcode == 13 && !inst.set_flags &&
        inst.shift == arm::ShiftKind::Ror) {
        // ROR uses the low byte modulo 32; RORV has the same low-five-bit
        // count. All inputs are read before a possibly aliased destination.
        code_.RORV(WReg { static_cast<int>(inst.rd) },
            WReg { static_cast<int>(inst.rm) },
            WReg { static_cast<int>(inst.rs) });
        return true;
    }
    if (inst.register_shift && inst.opcode == 13 && !inst.set_flags &&
        shift_range_ && shift_range_->accepts(inst)) {
        bounded_shift(inst, WReg { static_cast<int>(inst.rd) },
            WReg { static_cast<int>(inst.rm) });
        return true;
    }
    if (inst.immediate_operand || inst.register_shift ||
        (inst.shift != arm::ShiftKind::Lsl && inst.shift_amount == 0))
        // A32 LSR/ASR #32 and RRX require their distinct semantics.
        return false;
    const bool logical =
        inst.opcode == 0 || inst.opcode == 1 || inst.opcode >= 12;
    const bool arithmetic = inst.opcode == 2 || inst.opcode == 4 ||
                            inst.opcode == 10 || inst.opcode == 11;
    if ((!logical || inst.set_flags) &&
        (!arithmetic || inst.shift == arm::ShiftKind::Ror))
        return false;
    const auto a = lhs(inst, pc), b = reg(inst.rm, pc, W16);
    const WReg d = inst.opcode == 10 || inst.opcode == 11
                       ? WZR
                       : WReg { static_cast<int>(inst.rd) };
    const auto shift = static_cast<LogShift>(inst.shift);
    const auto arithmetic_shift = static_cast<AddSubShift>(inst.shift);
    const Imm<5> amount { inst.shift_amount };
    switch (inst.opcode) {
    case 0:
        code_.AND(d, a, b, shift, amount);
        break;
    case 1:
        code_.EOR(d, a, b, shift, amount);
        break;
    case 2:
    case 10:
        if (inst.set_flags)
            code_.SUBS(d, a, b, arithmetic_shift, amount);
        else
            code_.SUB(d, a, b, arithmetic_shift, amount);
        break;
    case 4:
    case 11:
        if (inst.set_flags)
            code_.ADDS(d, a, b, arithmetic_shift, amount);
        else
            code_.ADD(d, a, b, arithmetic_shift, amount);
        break;
    case 12:
        code_.ORR(d, a, b, shift, amount);
        break;
    case 13:
        code_.ORR(d, WZR, b, shift, amount);
        break;
    case 14:
        code_.BIC(d, a, b, shift, amount);
        break;
    case 15:
        code_.MVN(d, b, shift, amount);
        break;
    case arm::opcode_orn:
        code_.ORN(d, a, b, shift, amount);
        break;
    default:
        return false;
    }
    return true;
}

void IntegerEmitter::alu(const arm::Instruction& inst, std::uint32_t pc,
    LogicalFlagOutput flags)
{
    // The compiler may prove a test/compare's only result (NZCV) dead.
    if (!inst.set_flags && inst.opcode >= 8 && inst.opcode <= 11)
        return;
    pc = (pc + inst.pc_offset) & (inst.align_pc ? ~3U : ~0U);
    if (inst.register_shift && inst.opcode == 13 && inst.set_flags &&
        flags == LogicalFlagOutput::CarryOnly && shift_range_ &&
        shift_range_->precise() && shift_range_->accepts(inst)) {
        // Read original operands before overwriting an aliased destination;
        // ROR derives carry from its result. The count cannot be written.
        bounded_shifter(inst, WReg { static_cast<int>(inst.rd) },
            WReg { static_cast<int>(inst.rm) }, true, flags);
        return;
    }
    if (shifted_alu(inst, pc))
        return;
    const bool logical = inst.opcode == 0 || inst.opcode == 1 ||
                         inst.opcode == 8 || inst.opcode == 9 ||
                         inst.opcode >= 12;
    const WReg d = inst.opcode >= 8 && inst.opcode <= 11
                       ? WReg { 31 }
                       : WReg { static_cast<int>(inst.rd) };
    if (inst.immediate_operand &&
        (inst.opcode == 2 || inst.opcode == 4 || inst.opcode == 10 ||
            inst.opcode == 11) &&
        AddSubImm::is_valid(inst.immediate)) {
        const auto a = reg(inst.rn, pc, W15);
        if (inst.opcode == 2 || inst.opcode == 10) {
            if (inst.set_flags)
                code_.SUBS(d, a, AddSubImm { inst.immediate });
            else
                code_.SUB(d, a, AddSubImm { inst.immediate });
        } else {
            if (inst.set_flags)
                code_.ADDS(d, a, AddSubImm { inst.immediate });
            else
                code_.ADD(d, a, AddSubImm { inst.immediate });
        }
        return;
    }
    if (inst.immediate_operand && !inst.set_flags &&
        (inst.opcode == 13 || inst.opcode == 15)) {
        code_.MOV(d, inst.opcode == 13 ? inst.immediate : ~inst.immediate);
        return;
    }
    // The fast paths above leave X17 intact. General shifter/flag emission
    // may overwrite it, so later multiplies must reconstruct their addend.
    clear_multiply_addend();
    if (logical && logical_immediate(inst, pc, flags))
        return;
    const bool combined_nz = logical && inst.set_flags &&
        flags == LogicalFlagOutput::All &&
        (inst.opcode == 0 || inst.opcode == 8 || inst.opcode == 13 || inst.opcode == 14);
    const bool simple_operand =
        !inst.immediate_operand && !inst.register_shift &&
        inst.shift == arm::ShiftKind::Lsl && inst.shift_amount == 0;
    WReg b = W16;
    if (simple_operand) {
        if (logical && inst.set_flags && flags == LogicalFlagOutput::All)
            code_.MRS(X21, SystemReg::NZCV);
        b = reg(inst.rm, pc, W16);
    }
    else
        shifter(inst, pc, logical && inst.set_flags, flags);
    const auto a = lhs(inst, pc);
    switch (inst.opcode) {
    case 0:
    case 8:
        if (combined_nz)
            code_.ANDS(d, a, b);
        else
            code_.AND(d.index() == 31 ? W15 : d, a, b);
        break;
    case 1:
    case 9:
        code_.EOR(d.index() == 31 ? W15 : d, a, b);
        break;
    case 2:
    case 10:
        if (inst.set_flags)
            code_.SUBS(d, a, b);
        else
            code_.SUB(d, a, b);
        break;
    case 3:
        if (inst.set_flags)
            code_.SUBS(d, b, a);
        else
            code_.SUB(d, b, a);
        break;
    case 4:
    case 11:
        if (inst.set_flags)
            code_.ADDS(d, a, b);
        else
            code_.ADD(d, a, b);
        break;
    case 5:
        if (inst.set_flags)
            code_.ADCS(d, a, b);
        else
            code_.ADC(d, a, b);
        break;
    case 6:
        if (inst.set_flags)
            code_.SBCS(d, a, b);
        else
            code_.SBC(d, a, b);
        break;
    case 7:
        if (inst.set_flags)
            code_.SBCS(d, b, a);
        else
            code_.SBC(d, b, a);
        break;
    case 12:
        code_.ORR(d, a, b);
        break;
    case 13:
        if (combined_nz)
            code_.ANDS(d, b, b);
        else
            code_.MOV(d, b);
        break;
    case 14:
        if (combined_nz)
            code_.BICS(d, a, b);
        else
            code_.BIC(d, a, b);
        break;
    case 15:
        code_.MVN(d, b);
        break;
    case arm::opcode_orn:
        code_.ORN(d, a, b);
        break;
    }
    if (logical && inst.set_flags && flags == LogicalFlagOutput::All) {
        const auto value = d.index() == 31 ? W15 : d;
        if (!combined_nz)
            code_.ANDS(WZR, value, value);
        merge_nz();
    }
}
void IntegerEmitter::pack_halfword(const arm::Instruction& inst)
{
    shifter(inst, 0, false);
    const WReg a { static_cast<int>(inst.rn) },
        d { static_cast<int>(inst.rd) };
    if (inst.shift == arm::ShiftKind::Lsl) {
        code_.BFI(W16, a, 0, 16);
        code_.MOV(d, W16);
    } else {
        code_.MOV(d, a);
        code_.BFI(d, W16, 0, 16);
    }
}
void IntegerEmitter::wide_immediate(const arm::Instruction& inst)
{
    const WReg destination { static_cast<int>(inst.rd) };
    if (inst.opcode != 0)
        code_.MOVK(destination, { static_cast<std::uint16_t>(inst.immediate),
                                    MovImm16Shift::SHL_16 });
    else
        code_.MOV(destination, inst.immediate);
}

}
