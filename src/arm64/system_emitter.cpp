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
