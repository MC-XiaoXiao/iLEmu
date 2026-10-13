/* SPDX-License-Identifier: MPL-2.0 */
#include "flag_effects.hpp"

namespace ilemu::execution::arm64 {
namespace {
    constexpr unsigned n = 8, z = 4, c = 2, v = 1;
    unsigned condition_reads(unsigned condition)
    {
        switch (condition >> 1U) {
        case 0: return z;
        case 1: return c;
        case 2: return n;
        case 3: return v;
        case 4: return c | z;
        case 5: return n | v;
        case 6: return n | v | z;
        default: return 0;
        }
    }
}
FlagEffects flag_effects(const arm::Instruction& inst) noexcept
{
    FlagEffects out;
    out.read = condition_reads(inst.condition);
    if (inst.condition == 15)
        return out;
    using Kind = arm::InstructionKind;
    switch (inst.kind) {
    case Kind::DataProcessing: {
        const bool test = inst.opcode >= 8 && inst.opcode <= 11;
        if (!test && inst.rd == 15)
            return out;
        const bool arithmetic = (inst.opcode >= 2 && inst.opcode <= 7) ||
                                inst.opcode == 10 || inst.opcode == 11;
        if ((inst.opcode >= 5 && inst.opcode <= 7) ||
            (!inst.immediate_operand && !inst.register_shift &&
                inst.shift == arm::ShiftKind::Ror && inst.shift_amount == 0))
            out.read |= c;
        if (inst.set_flags) {
            out.possible_write = arithmetic ? n | z | c | v : n | z;
            out.guaranteed_write = out.possible_write;
            if (!arithmetic) {
                const bool carry_changes = inst.immediate_operand
                    ? inst.shift_amount != 0
                    : inst.register_shift || inst.shift != arm::ShiftKind::Lsl ||
                          inst.shift_amount != 0;
                if (carry_changes) {
                    out.possible_write |= c;
                    if (!inst.register_shift)
                        out.guaranteed_write |= c;
                }
            }
        }
        break;
    }
    case Kind::Multiply:
        if (inst.set_flags)
            out.possible_write = out.guaranteed_write = n | z;
        break;
    case Kind::FloatingSystem: {
        const auto& operands = std::get<arm::FloatingSystemOperands>(inst.vector);
        if (operands.to_core && operands.core == 15)
            out.possible_write = out.guaranteed_write = n | z | c | v;
        break;
    }
    case Kind::SystemControl:
        if (inst.system != arm::SystemOperation::ProgramStatusRegister ||
            (!inst.load && (inst.opcode & 2U) != 0))
            return out;
        if (inst.load)
            out.read |= n | z | c | v;
        else if ((inst.opcode & 8U) != 0)
            out.possible_write = out.guaranteed_write = n | z | c | v;
        break;
    case Kind::IfThen:
    case Kind::Nop:
    case Kind::WideImmediate:
    case Kind::PackHalfword:
    case Kind::IntegerTransform:
    case Kind::IntegerDivide:
    case Kind::ParallelSaturating:
    case Kind::ParallelArithmetic:
    case Kind::VectorDuplicate:
    case Kind::VectorBitwise:
    case Kind::VectorExtract:
    case Kind::VectorImmediate:
    case Kind::VectorCoreMove:
        break;
    default:
        // Faults, checked memory, control flow and unknown operations expose
        // the exact paid prefix; they cannot be crossed by a liveness proof.
        return out;
    }
    if (inst.condition != 14)
        out.guaranteed_write = 0;
    out.boundary = false;
    return out;
}
}
