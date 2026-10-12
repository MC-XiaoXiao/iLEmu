/* SPDX-License-Identifier: MPL-2.0 */
#include "system_emitter.hpp"
#include "compiler.hpp"
#include <cstddef>
namespace ilemu::execution::arm64 {
void SystemEmitter::emit(const arm::Instruction& inst)
{
    using namespace oaknut;
    using namespace oaknut::util;
    const WReg value { static_cast<int>(inst.rd) };
    switch (inst.system) {
    case arm::SystemOperation::ProgramStatusRegister:
        if (inst.load) {
            code_.MOV(W16, 0x08ff03dfU);
            code_.AND(W17, W22, W16);
            code_.MRS(X16, SystemReg::NZCV);
            code_.ORR(value, W17, W16);
        } else {
            const auto input = inst.immediate_operand ? W17 : value;
            if (inst.immediate_operand)
                code_.MOV(input, inst.immediate);
            if ((inst.opcode & 8U) != 0) {
                code_.MSR(SystemReg::NZCV, XReg { input.index() });
                code_.UBFX(W16, input, 27, 1);
                code_.BFI(W22, W16, 27, 1);
            }
            if ((inst.opcode & 4U) != 0) {
                code_.UBFX(W16, input, 16, 4);
                code_.BFI(W22, W16, 16, 4);
            }
            if ((inst.opcode & 2U) != 0) {
                code_.UBFX(W16, input, 9, 1);
                code_.BFI(W22, W16, 9, 1);
            }
        }
        break;
    case arm::SystemOperation::ThreadPointerReadWrite:
        if (inst.load)
            code_.LDR(value, X19, offsetof(CpuThreadState, thread_pointer_user_rw));
        else
            code_.STR(value, X19, offsetof(CpuThreadState, thread_pointer_user_rw));
        break;
    case arm::SystemOperation::ThreadPointerReadOnly:
        code_.LDR(value, X20, offsetof(NativeOutcome, thread_pointer_read_only));
        break;
    case arm::SystemOperation::DataMemoryBarrier:
        code_.DMB(BarrierOp::SY);
        break;
    case arm::SystemOperation::DataSynchronizationBarrier:
        code_.DSB(BarrierOp::SY);
        break;
    case arm::SystemOperation::InstructionSynchronizationBarrier:
        code_.ISB();
        break;
    }
}
}
