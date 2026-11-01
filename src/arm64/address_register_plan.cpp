/* SPDX-License-Identifier: MPL-2.0 */
#include "address_register_plan.hpp"
#include "address_displacements.hpp"
namespace ilemu::execution::arm64 {
bool AddressRegisterPlan::update(const arm::Instruction& inst)
{
    // Bound constants and coefficients so modulo32 displacement proofs cannot
    // wrap while this cycle is lowered to signed64 pointer arithmetic.
    return inst.kind == arm::InstructionKind::DataProcessing &&
        inst.condition == 14 && !inst.set_flags && inst.rd == inst.rn &&
        inst.rd < 15 && (inst.opcode == 2 || inst.opcode == 4) &&
        (inst.immediate_operand ? inst.immediate <= 4095 :
            !inst.register_shift && inst.shift == arm::ShiftKind::Lsl &&
            inst.shift_amount <= 3 && inst.rm < 15 && inst.rm != inst.rd);
}
void AddressRegisterPlan::Builder::observe(
    const arm::Instruction& inst, bool addressable)
{
    using Kind = arm::InstructionKind;
    switch (inst.kind) {
    case Kind::DataProcessing:
        if (update(inst)) {
            if (!inst.immediate_operand) {
                forbid(inst.rm);
                variable_updates_ |= 1U << inst.rd;
            }
            return;
        }
        for_written_register(inst, [&](unsigned reg) { forbid(reg); });
        if (inst.opcode != 13 && inst.opcode != 15)
            forbid(inst.rn);
        if (!inst.immediate_operand) {
            forbid(inst.rm);
            if (inst.register_shift)
                forbid(inst.rs);
        }
        return;
    case Kind::VectorTransfer: {
        const auto change = AddressDisplacements::writeback(inst);
        if (!addressable || inst.condition != 14 ||
            (inst.writeback && (!change || change->delta < -4095 ||
                change->delta > 4095)))
            forbid(inst.rn);
        const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
        if (v.mode != arm::VectorTransferMode::ScalarMultiple &&
            inst.rm != 13 && inst.rm != 15)
            forbid(inst.rm);
        return;
    }
    case Kind::VectorDuplicate: {
        const auto& v = std::get<arm::VectorDuplicateOperands>(inst.vector);
        if (v.core_source)
            forbid(v.source);
        return;
    }
    case Kind::VectorCoreMove: {
        const auto& v = std::get<arm::VectorCoreMoveOperands>(inst.vector);
        forbid(v.core);
        if (v.pair)
            forbid(v.core2);
        return;
    }
    case Kind::FloatingSystem:
        forbid(std::get<arm::FloatingSystemOperands>(inst.vector).core);
        return;
    case Kind::Branch:
        if (inst.link)
            forbid(14);
        return;
    case Kind::FloatingArithmetic:
    case Kind::FloatingComparison:
    case Kind::FloatingConversion:
    case Kind::FloatingImmediate:
    case Kind::VectorFloatingConversion:
    case Kind::VectorFloatingArithmetic:
    case Kind::VectorIntegerArithmetic:
    case Kind::VectorBitwise:
    case Kind::VectorShiftImmediate:
    case Kind::VectorReverse:
    case Kind::VectorExtract:
    case Kind::VectorImmediate:
    case Kind::Nop:
    case Kind::IfThen:
        return;
    default:
        forbidden_ = 0xffff;
    }
}
}
