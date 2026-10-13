/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/saturating_arithmetic.hpp"
#include "integer_emitter.hpp"

namespace ilemu::execution::arm64 {
void IntegerEmitter::saturating_arithmetic(const arm::Instruction& inst)
{
    using namespace oaknut;
    using namespace oaknut::util;
    using Op = arm::SaturatingOperation;
    const auto operation = static_cast<Op>(inst.opcode);
    const WReg first { static_cast<int>(inst.rn) },
        second { static_cast<int>(inst.rm) }, rd { static_cast<int>(inst.rd) };
    const auto saturate = [&](unsigned bits, bool is_signed) {
        const auto maximum =
            (std::uint64_t { 1 } << (bits - unsigned(is_signed))) - 1;
        const auto minimum =
            is_signed ? -(std::uint64_t { 1 } << (bits - 1U)) : 0;
        code_.MOV(X17, maximum);
        code_.CMP(X16, X17);
        code_.CSET(W15, Cond::GT);
        code_.CSEL(X16, X17, X16, Cond::GT);
        code_.ORR(W22, W22, W15, LogShift::LSL, 27);
        code_.MOV(X17, minimum);
        code_.CMP(X16, X17);
        code_.CSET(W15, Cond::LT);
        code_.CSEL(X16, X17, X16, Cond::LT);
        code_.ORR(W22, W22, W15, LogShift::LSL, 27);
    };
    // Saturation changes only sticky Q; all host flag comparisons are private.
    code_.MRS(X21, SystemReg::NZCV);
    if (operation == Op::Signed || operation == Op::Unsigned) {
        if (inst.shift == arm::ShiftKind::Asr)
            code_.ASR(
                W16, first, inst.shift_amount == 0 ? 31 : inst.shift_amount);
        else
            code_.LSL(W16, first, inst.shift_amount);
        code_.SXTW(X16, W16);
        saturate(inst.immediate, operation == Op::Signed);
        code_.MOV(rd, W16);
    } else if (operation == Op::SignedHalfwords ||
               operation == Op::UnsignedHalfwords) {
        code_.SBFX(X16, first.toX(), 0, 16);
        saturate(inst.immediate, operation == Op::SignedHalfwords);
        code_.AND(W16, W16, 0xffffU);
        // V0 is existing scratch, preserving an aliased source's high half.
        code_.FMOV(S0, W16);
        code_.SBFX(X16, first.toX(), 16, 16);
        saturate(inst.immediate, operation == Op::SignedHalfwords);
        code_.FMOV(W17, S0);
        code_.ORR(rd, W17, W16, LogShift::LSL, 16);
    } else {
        code_.SXTW(X16, second);
        if (operation == Op::DoubleAdd || operation == Op::DoubleSubtract) {
            code_.ADD(X16, X16, X16);
            saturate(32, true);
        }
        code_.SXTW(X17, first);
        if (operation == Op::Subtract || operation == Op::DoubleSubtract)
            code_.SUB(X16, X17, X16);
        else
            code_.ADD(X16, X17, X16);
        saturate(32, true);
        code_.MOV(rd, W16);
    }
    code_.MSR(SystemReg::NZCV, X21);
}
}
