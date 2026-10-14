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
    // GE lives in the cached CPSR. Keep host NZCV unchanged and write the
    // destination only after every source lane is read.
    if (operation == Op::Select) {
        // Spread the four GE bits to byte low bits, then expand to byte masks.
        code_.UBFX(W16, W22, 16, 4);
        code_.MOV(W17, 0x00204081U);
        code_.MUL(W16, W16, W17);
        code_.AND(W16, W16, 0x01010101U);
        code_.LSL(W17, W16, 8);
        code_.SUB(W16, W17, W16);
        code_.EOR(W17, n, m);
        code_.AND(W17, W17, W16);
        code_.EOR(WReg {static_cast<int>(inst.rd)}, m, W17);
    } else {
        const auto layout = arm::parallel_arithmetic_layout(operation);
        if (layout.bits == 8) {
            parallel_byte_arithmetic(inst, layout.subtract(0));
            return;
        }
        code_.MOV(W15, 0);
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
            if (!inst.sign_extend && !subtract) {
                code_.UBFX(W16, W17, layout.bits, 1);
                if (layout.bits == 16)
                    code_.NEG(W16, W16);
            } else {
                // A widened lane result fits signed 32 bits. Its inverted
                // sign gives GE, repeated for both bits of a halfword lane.
                if (layout.bits == 16) {
                    code_.ASR(W16, W17, 31);
                    code_.MVN(W16, W16);
                } else {
                    code_.LSR(W16, W17, 31);
                    code_.EOR(W16, W16, 1U);
                }
            }
            code_.BFI(W22, W16, 16U + offset / 8U, layout.bits / 8U);
        }
        code_.MOV(WReg {static_cast<int>(inst.rd)}, W15);
    }
}
}
