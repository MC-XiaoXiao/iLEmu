/* SPDX-License-Identifier: MPL-2.0 */
#include "integer_emitter.hpp"
#include "arm/multiply.hpp"
namespace ilemu::execution::arm64 {
bool IntegerEmitter::dual_halfword_multiply(const arm::Instruction& inst)
{
    using namespace oaknut;
    using namespace oaknut::util;
    using Op = arm::MultiplyOperation;
    const auto operation = static_cast<Op>(inst.opcode);
    if (!arm::dual_halfword_multiply(operation))
        return false;
    const WReg rd {static_cast<int>(inst.rd)}, rn {static_cast<int>(inst.rn)},
        rm {static_cast<int>(inst.rm)}, rs {static_cast<int>(inst.rs)};
    // V0..V2 are trace scratch, distinct from all guest floating-point banks.
    // Integer widening multiplication does not change host FPCR/FPSR.
    code_.FMOV(S0, rm);
    code_.FMOV(S1, rs);
    if (inst.exchange)
        code_.REV32(D1.H4(), D1.H4());
    code_.SMULL(Q2.S4(), D0.H4(), D1.H4());
    code_.SMOV(X16, Q2.Selem()[0]);
    code_.SMOV(X17, Q2.Selem()[1]);
    const bool subtract = operation == Op::DualHalfwordSubtract ||
                          operation == Op::DualHalfwordLongSubtract;
    if (subtract) code_.SUB(X17, X16, X17);
    else code_.ADD(X17, X16, X17);
    if (arm::multiply_writes_pair(operation)) {
        code_.MOV(W16, rd);
        code_.ORR(X16, X16, rn.toX(), LogShift::LSL, 32);
        code_.ADD(X17, X17, X16);
        code_.LSR(X16, X17, 32);
        code_.MOV(rd, W17);
        code_.MOV(rn, W16);
    } else {
        if (inst.accumulate) {
            code_.SXTW(X16, rn);
            code_.ADD(X17, X17, X16);
        }
        if (!subtract || inst.accumulate) {
            // Test the complete wide result so accumulator cancellation is
            // honored. Keep NZCV intact and merge only sticky Q into CPSR.
            code_.MRS(X21, SystemReg::NZCV);
            code_.SXTW(X16, W17);
            code_.CMP(X17, X16);
            code_.CSET(W16, Cond::NE);
            code_.ORR(W22, W22, W16, LogShift::LSL, 27);
            code_.MSR(SystemReg::NZCV, X21);
        }
        code_.MOV(rd, W17);
    }
    return true;
}
}
