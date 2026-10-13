/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/parallel_arithmetic.hpp"
#include "integer_emitter.hpp"
namespace ilemu::execution::arm64 {
void IntegerEmitter::parallel_arithmetic(const arm::Instruction& inst)
{
    using namespace oaknut;
    using namespace oaknut::util;
    using Op = arm::ParallelArithmeticOperation;
    const auto operation = static_cast<Op>(inst.opcode);
    const WReg n {static_cast<int>(inst.rn)}, m {static_cast<int>(inst.rm)};
    // GE lives in the cached CPSR. Temporary host comparisons must not change
    // guest NZCV; write the destination only after every source lane is read.
    code_.MRS(X21, SystemReg::NZCV);
    code_.MOV(W15, 0);
    if (operation == Op::Select) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            code_.TST(W22, 1U << (16U + byte));
            code_.CSEL(W16, n, m, Cond::NE);
            code_.UBFX(W16, W16, byte * 8U, 8);
            code_.BFI(W15, W16, byte * 8U, 8);
        }
    } else {
        const auto layout = arm::parallel_arithmetic_layout(operation);
        for (unsigned offset = 0; offset < 32; offset += layout.bits) {
            const auto second_offset = layout.exchange ? offset ^ 16U : offset;
            if (inst.sign_extend) {
                code_.SBFX(W16, n, offset, layout.bits);
                code_.SBFX(W17, m, second_offset, layout.bits);
            } else {
                code_.UBFX(W16, n, offset, layout.bits);
                code_.UBFX(W17, m, second_offset, layout.bits);
            }
            const bool subtract = layout.subtract(offset);
            if (subtract) code_.SUB(W17, W16, W17);
            else code_.ADD(W17, W16, W17);
            code_.BFI(W15, W17, offset, layout.bits);
            if (!inst.sign_extend && !subtract)
                code_.UBFX(W16, W17, layout.bits, 1);
            else {
                code_.CMP(W17, 0);
                code_.CSET(W16, Cond::GE);
            }
            if (layout.bits == 16)
                code_.NEG(W16, W16);
            code_.BFI(W22, W16, 16U + offset / 8U, layout.bits / 8U);
        }
    }
    code_.MSR(SystemReg::NZCV, X21);
    code_.MOV(WReg {static_cast<int>(inst.rd)}, W15);
}
}
