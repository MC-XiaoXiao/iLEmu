/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/saturating_arithmetic.hpp"
#include "integer_emitter.hpp"

namespace ilemu::execution::arm64 {
void IntegerEmitter::saturating_arithmetic(const arm::Instruction& inst,
    bool preserve_nzcv)
{
    using namespace oaknut;
    using namespace oaknut::util;
    using Op = arm::SaturatingOperation;
    const auto operation = static_cast<Op>(inst.opcode);
    const WReg first { static_cast<int>(inst.rn) },
        second { static_cast<int>(inst.rm) }, rd { static_cast<int>(inst.rd) };
    const auto shifted_source = [&](WReg destination) {
        if (inst.shift == arm::ShiftKind::Asr)
            code_.ASR(destination, first,
                inst.shift_amount == 0 ? 31 : inst.shift_amount);
        else
            code_.LSL(destination, first, inst.shift_amount);
    };
    // Shifts wrap to a signed word before SSAT; a full-width signed result
    // cannot saturate. SSAT16 at its full width is likewise an identity.
    if (operation == Op::Signed && inst.immediate == 32) {
        shifted_source(rd);
        return;
    }
    if (operation == Op::SignedHalfwords && inst.immediate == 16) {
        code_.MOV(rd, first);
        return;
    }
    const bool halfwords = operation == Op::SignedHalfwords ||
                           operation == Op::UnsignedHalfwords;
    const bool unsigned_result = operation == Op::Unsigned ||
                                 operation == Op::UnsignedHalfwords;
    // At the largest unsigned width, only negative signed inputs can clamp.
    const bool save_flags = preserve_nzcv && (!unsigned_result ||
        inst.immediate < (halfwords ? 15U : 31U));
    if (save_flags)
        code_.MRS(X21, SystemReg::NZCV);
    if (operation == Op::Signed || operation == Op::Unsigned) {
        shifted_source(W16);
        code_.SXTW(X16, W16);
        saturate_scalar(inst.immediate, operation == Op::Signed, 32);
        code_.MOV(rd, W16);
    } else if (operation == Op::SignedHalfwords ||
               operation == Op::UnsignedHalfwords) {
        code_.SBFX(X16, first.toX(), 0, 16);
        saturate_scalar(inst.immediate, operation == Op::SignedHalfwords, 16);
        code_.AND(W16, W16, 0xffffU);
        // V0 is existing scratch, preserving an aliased source's high half.
        code_.FMOV(S0, W16);
        code_.SBFX(X16, first.toX(), 16, 16);
        saturate_scalar(inst.immediate, operation == Op::SignedHalfwords, 16);
        code_.FMOV(W17, S0);
        code_.ORR(rd, W17, W16, LogShift::LSL, 16);
    } else {
        code_.SXTW(X16, second);
        if (operation == Op::DoubleAdd || operation == Op::DoubleSubtract) {
            code_.ADD(X16, X16, X16);
            saturate_scalar(32, true, 33);
        }
        code_.SXTW(X17, first);
        if (operation == Op::Subtract || operation == Op::DoubleSubtract)
            code_.SUB(X16, X17, X16);
        else
            code_.ADD(X16, X17, X16);
        saturate_scalar(32, true, 33);
        code_.MOV(rd, W16);
    }
    if (save_flags)
        code_.MSR(SystemReg::NZCV, X21);
}
}
