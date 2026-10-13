/* SPDX-License-Identifier: MPL-2.0 */
#include "integer_emitter.hpp"
#include "arm/multiply.hpp"
namespace ilemu::execution::arm64 {
bool IntegerEmitter::dsp_multiply(const arm::Instruction& inst)
{
    using namespace oaknut;
    using namespace oaknut::util;
    using Operation = arm::MultiplyOperation;
    const auto operation = static_cast<Operation>(inst.opcode);
    if (operation != Operation::Halfword && operation != Operation::HalfwordLong &&
        operation != Operation::WordHalfword)
        return false;
    const WReg rd {static_cast<int>(inst.rd)}, rn {static_cast<int>(inst.rn)},
        rm {static_cast<int>(inst.rm)}, rs {static_cast<int>(inst.rs)};
    const auto finish_word = [&] {
        if (inst.accumulate) {
            // DSP accumulation sets sticky Q on signed overflow; preserve
            // guest NZCV even though the host add needs its overflow flag.
            code_.MRS(X21, SystemReg::NZCV);
            code_.ADDS(rd, W17, rn);
            code_.CSET(W16, Cond::VS);
            code_.ORR(W22, W22, W16, LogShift::LSL, 27);
            code_.MSR(SystemReg::NZCV, X21);
        } else
            code_.MOV(rd, W17);
    };
    code_.SBFX(W17, rs, (inst.shift_amount & 2U) != 0 ? 16 : 0, 16);
    if (operation == Operation::WordHalfword) {
        code_.SMULL(X17, rm, W17);
        code_.ASR(X17, X17, 16);
        finish_word();
    } else {
        code_.SBFX(W16, rm, (inst.shift_amount & 1U) != 0 ? 16 : 0, 16);
        if (operation == Operation::HalfwordLong) {
            code_.SMULL(X16, W16, W17);
            code_.MOV(W17, rd);
            code_.ORR(X17, X17, rn.toX(), LogShift::LSL, 32);
            code_.ADD(X16, X16, X17);
            // Consume both sources and the original pair before either write.
            code_.LSR(X17, X16, 32);
            code_.MOV(rd, W16);
            code_.MOV(rn, W17);
        } else if (inst.accumulate) {
            code_.MUL(W17, W16, W17);
            finish_word();
        } else
            code_.MUL(rd, W16, W17);
    }
    return true;
}
}
