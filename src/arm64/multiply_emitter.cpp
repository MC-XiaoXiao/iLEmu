/* SPDX-License-Identifier: MPL-2.0 */
#include "integer_emitter.hpp"
#include "arm/multiply.hpp"

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
void IntegerEmitter::multiply(const arm::Instruction& inst)
{
    using Operation = arm::MultiplyOperation;
    const WReg rd { static_cast<int>(inst.rd) }, rn { static_cast<int>(inst.rn) },
        rm { static_cast<int>(inst.rm) }, rs { static_cast<int>(inst.rs) };
    const auto operation = static_cast<Operation>(inst.opcode);
    if (operation == Operation::High || operation == Operation::HighSubtract) {
        if (inst.condition != 14)
            clear_multiply_addend();
        const unsigned addend_reg = inst.accumulate ? inst.rn : 16U;
        XReg addend = XZR;
        if (inst.shift_amount != 0 || inst.accumulate) {
            if (!multiply_addend_ || multiply_addend_->reg != addend_reg ||
                multiply_addend_->round != (inst.shift_amount != 0)) {
                if (inst.shift_amount != 0) {
                    code_.MOV(W17, 0x80000000U);
                    if (inst.accumulate)
                        code_.ORR(X17, X17, rn.toX(), LogShift::LSL, 32);
                } else
                    code_.LSL(X17, rn.toX(), 32);
                multiply_addend_ = MultiplyAddend {
                    addend_reg, inst.shift_amount != 0 };
            }
            addend = X17;
        }
        if (operation == Operation::HighSubtract)
            code_.SMSUBL(X16, rm, rs, addend);
        else
            code_.SMADDL(X16, rm, rs, addend);
        code_.LSR(rd.toX(), X16, 32);
        // X17 survives adjacent unconditional high-word operations, but a
        // conditional path or a write to its source revokes that knowledge.
        if (inst.condition != 14 ||
            (multiply_addend_ && multiply_addend_->reg == inst.rd))
            clear_multiply_addend();
        return;
    }
    clear_multiply_addend();
    if (!arm::multiply_writes_pair(inst)) {
        if (operation == Operation::Subtract)
            code_.MSUB(rd, rm, rs, rn);
        else if (inst.accumulate)
            code_.MADD(rd, rm, rs, rn);
        else
            code_.MUL(rd, rm, rs);
        if (inst.set_flags) {
            code_.MRS(X21, SystemReg::NZCV);
            code_.ANDS(WZR, rd, rd);
            merge_nz();
        }
        return;
    }
    XReg addend = XZR;
    if (operation == Operation::DualAccumulateLong) {
        code_.ADD(X16, rd.toX(), rn.toX());
        addend = X16;
    } else if (inst.accumulate) {
        code_.MOV(W16, rd);
        code_.ORR(X16, X16, rn.toX(), LogShift::LSL, 32);
        addend = X16;
    }
    if (inst.sign_extend)
        code_.SMADDL(X16, rm, rs, addend);
    else
        code_.UMADDL(X16, rm, rs, addend);
    if (inst.set_flags) {
        code_.MRS(X21, SystemReg::NZCV);
        code_.ANDS(XZR, X16, X16);
        merge_nz();
    }
    // Both original inputs/addends are consumed before either destination is
    // changed, including aliases with either half of the accumulator.
    code_.LSR(X17, X16, 32);
    code_.MOV(rd, W16);
    code_.MOV(rn, W17);
}
}
