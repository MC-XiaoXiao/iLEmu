/* SPDX-License-Identifier: MPL-2.0 */
#include "exclusive_emitter.hpp"
#include "compiler.hpp"
#include <cstddef>
namespace ilemu::execution::arm64 {
void ExclusiveEmitter::emit(const arm::Instruction& inst, std::uint64_t ticks,
    oaknut::Label& checked)
{
    using namespace oaknut;
    using namespace oaknut::util;
    constexpr auto offset = offsetof(NativeOutcome, exclusive);
    code_.MOV(X16, ticks);
    code_.STR(X16, X20, offset + offsetof(arm_memory::ExclusiveTransfer, ticks));
    code_.MOV(W16, arm_memory::exclusive_control(inst));
    code_.STR(W16, X20, offset + offsetof(arm_memory::ExclusiveTransfer, control));
    if (inst.kind != arm::InstructionKind::ClearExclusive) {
        code_.ADD(W16, WReg { static_cast<int>(inst.rn) }, inst.immediate);
        code_.STR(W16, X20, offset + offsetof(arm_memory::ExclusiveTransfer, address));
        if (!inst.load) {
            code_.MOV(W21, WReg { static_cast<int>(inst.rm) });
            if (inst.access_size == 8)
                code_.ORR(X21, X21, XReg { static_cast<int>(inst.rs) }, LogShift::LSL, 32);
            code_.STR(X21, X20, offset + offsetof(arm_memory::ExclusiveTransfer, value));
        }
    }
    // Code/page leases must end before the adapter takes its physical writer
    // lock. Publish live registers through the ordinary precise checked exit.
    code_.B(checked);
}
}
