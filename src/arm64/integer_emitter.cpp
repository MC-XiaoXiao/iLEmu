/* SPDX-License-Identifier: MPL-2.0 */
#include "integer_emitter.hpp"
namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
void IntegerEmitter::merge_nz()
{
    code_.MRS(X17, SystemReg::NZCV);
    code_.AND(W21, W21, 0x3fffffffU);
    code_.AND(W17, W17, 0xc0000000U);
    code_.ORR(W21, W21, W17);
    code_.MSR(SystemReg::NZCV, X21);
}
WReg IntegerEmitter::reg(unsigned index, std::uint32_t pc, WReg scratch)
{
    if (index != 15)
        return WReg { static_cast<int>(index) };
    code_.MOV(scratch, pc);
    return scratch;
}
void IntegerEmitter::carry_from(WReg value, unsigned bit)
{
    if (bit != 0)
        code_.LSR(W15, value, bit);
    else
        code_.MOV(W15, value);
    code_.BFI(X21, X15, 29, 1);
}
void IntegerEmitter::shifter(
    const arm::Instruction& inst, std::uint32_t pc, bool carry_output)
{
    if (carry_output)
        code_.MRS(X21, SystemReg::NZCV);
    if (inst.immediate_operand) {
        code_.MOV(W16, inst.immediate);
        if (carry_output && inst.shift_amount != 0)
            carry_from(W16, 31);
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
                    carry_from(rm, 32 - amount);
                code_.LSL(W16, rm, amount);
            }
            break;
        case arm::ShiftKind::Lsr:
            if (carry_output)
                carry_from(rm, amount == 0 ? 31 : amount - 1);
            if (amount == 0)
                code_.MOV(W16, 0);
            else
                code_.LSR(W16, rm, amount);
            break;
        case arm::ShiftKind::Asr:
            if (carry_output)
                carry_from(rm, amount == 0 ? 31 : amount - 1);
            code_.ASR(W16, rm, amount == 0 ? 31 : amount);
            break;
        case arm::ShiftKind::Ror:
            if (amount == 0) {
                if (!carry_output)
                    code_.MRS(X21, SystemReg::NZCV);
                code_.LSR(W17, W21,
                    29); // Preserve incoming C before replacing it.
                if (carry_output)
                    carry_from(rm, 0);
                code_.LSR(W16, rm, 1);
                code_.BFI(W16, W17, 31, 1);
            } else {
                if (carry_output)
                    carry_from(rm, amount - 1);
                code_.ROR(W16, rm, amount);
            }
            break;
        }
        return;
    }
    Label zero, large, done, carry_zero;
    code_.AND(W17, WReg { static_cast<int>(inst.rs) }, 255);
    code_.CBZ(W17, zero);
    if (inst.shift == arm::ShiftKind::Ror) {
        code_.RORV(W16, rm, W17);
        if (carry_output)
            carry_from(W16, 31);
    } else {
        code_.LSR(W15, W17, 5);
        code_.CBNZ(W15, large);
        if (carry_output) {
            if (inst.shift == arm::ShiftKind::Lsl)
                code_.NEG(W15, W17);
            else
                code_.SUB(W15, W17, 1);
            code_.LSRV(W15, rm, W15);
            code_.BFI(X21, X15, 29, 1);
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
                carry_from(rm, 31);
        } else {
            code_.MOV(W16, 0);
            if (carry_output) {
                code_.EOR(W15, W17, 32);
                code_.CBNZ(W15, carry_zero);
                carry_from(rm, inst.shift == arm::ShiftKind::Lsl ? 0 : 31);
                code_.B(done);
                code_.l(carry_zero);
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
    const auto a = reg(inst.rn, pc, W15), b = reg(inst.rm, pc, W16);
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

void IntegerEmitter::alu(const arm::Instruction& inst, std::uint32_t pc)
{
    pc = (pc + inst.pc_offset) & (inst.align_pc ? ~3U : ~0U);
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
    const bool simple_operand =
        !inst.immediate_operand && !inst.register_shift &&
        inst.shift == arm::ShiftKind::Lsl && inst.shift_amount == 0 &&
        !(logical && inst.set_flags);
    WReg b = W16;
    if (simple_operand)
        b = reg(inst.rm, pc, W16);
    else
        shifter(inst, pc, logical && inst.set_flags);
    const auto a = reg(inst.rn, pc, W15);
    switch (inst.opcode) {
    case 0:
    case 8:
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
        code_.MOV(d, b);
        break;
    case 14:
        code_.BIC(d, a, b);
        break;
    case 15:
        code_.MVN(d, b);
        break;
    case arm::opcode_orn:
        code_.ORN(d, a, b);
        break;
    }
    if (logical && inst.set_flags) {
        const auto value = d.index() == 31 ? W15 : d;
        code_.ANDS(WZR, value, value);
        merge_nz();
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
void IntegerEmitter::multiply(const arm::Instruction& inst)
{
    const WReg rd { static_cast<int>(inst.rd) },
        rm { static_cast<int>(inst.rm) }, rs { static_cast<int>(inst.rs) };
    if (inst.accumulate)
        code_.MADD(rd, rm, rs, WReg { static_cast<int>(inst.rn) });
    else
        code_.MUL(rd, rm, rs);
    if (inst.set_flags) {
        code_.MRS(X21, SystemReg::NZCV);
        code_.ANDS(WZR, rd, rd);
        merge_nz();
    }
}

}
