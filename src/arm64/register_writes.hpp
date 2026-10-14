/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "arm/multiply.hpp"
namespace ilemu::execution::arm64 {
// Shared architectural dependencies; host scratch clobbers stay with emitters.
template <typename Invalidate>
void for_written_register(const arm::Instruction& inst, Invalidate&& invalidate)
{
    if (inst.kind == arm::InstructionKind::FloatingSystem) {
        const auto& v = std::get<arm::FloatingSystemOperands>(inst.vector);
        if (v.to_core && v.core != 15)
            invalidate(v.core);
    }
    if (inst.kind == arm::InstructionKind::VectorCoreMove) {
        const auto& v = std::get<arm::VectorCoreMoveOperands>(inst.vector);
        if (v.to_core) {
            invalidate(v.core);
            if (v.pair)
                invalidate(v.core2);
        }
    }
    if ((inst.kind == arm::InstructionKind::DataProcessing &&
            (inst.opcode < 8 || inst.opcode > 11)) ||
        inst.kind == arm::InstructionKind::Multiply ||
        inst.kind == arm::InstructionKind::IntegerDivide ||
        inst.kind == arm::InstructionKind::PackHalfword ||
        inst.kind == arm::InstructionKind::IntegerTransform ||
        inst.kind == arm::InstructionKind::ParallelSaturating ||
        inst.kind == arm::InstructionKind::ParallelArithmetic ||
        inst.kind == arm::InstructionKind::SaturatingArithmetic ||
        inst.kind == arm::InstructionKind::WideImmediate ||
        inst.kind == arm::InstructionKind::Swap ||
        ((inst.kind == arm::InstructionKind::Transfer ||
             inst.kind == arm::InstructionKind::DualTransfer) &&
            inst.load) ||
        (inst.kind == arm::InstructionKind::SystemControl && inst.load))
        invalidate(inst.rd);
    if (inst.kind == arm::InstructionKind::DualTransfer && inst.load)
        invalidate(inst.rs);
    if (inst.kind == arm::InstructionKind::Multiply &&
        arm::multiply_writes_pair(inst))
        invalidate(inst.rn);
    if ((inst.kind == arm::InstructionKind::Transfer ||
            inst.kind == arm::InstructionKind::DualTransfer ||
            inst.kind == arm::InstructionKind::VectorTransfer) &&
        inst.writeback)
        invalidate(inst.rn);
    if ((inst.kind == arm::InstructionKind::Branch ||
            inst.kind == arm::InstructionKind::BranchExchange) &&
        inst.link)
        invalidate(14);
}
}
