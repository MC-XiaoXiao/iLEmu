/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "register_writes.hpp"
#include "arm_memory/vector_transfer.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
namespace ilemu::execution::arm64 {
// Compile-only, modulo-32-bit offsets from each invocation-entry register.
// Unknown or conditional writes cannot regain a proof from a later adjustment.
class AddressDisplacements {
public:
    struct Change { unsigned reg; std::int32_t delta; };
    struct Expression {
        std::int32_t constant = 0, coefficient = 0;
        std::optional<unsigned> index;
        std::optional<Expression> added(const Expression& other) const
        {
            if (index && other.index && index != other.index)
                return {};
            Expression result {
                std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(constant) +
                    static_cast<std::uint32_t>(other.constant)),
                std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(coefficient) +
                    static_cast<std::uint32_t>(other.coefficient)),
                index ? index : other.index };
            if (result.coefficient == 0)
                result.index.reset();
            return result;
        }
    };
    AddressDisplacements()
    {
        for (auto& value : values_)
            value = Expression {};
    }
    static std::optional<Change> writeback(const arm::Instruction& inst)
    {
        using Kind = arm::InstructionKind;
        if (!inst.writeback || inst.rn == 15)
            return {};
        std::uint32_t delta;
        if (inst.kind == Kind::Transfer || inst.kind == Kind::DualTransfer) {
            if (!inst.immediate_operand || (inst.load &&
                    (inst.rd == inst.rn ||
                        (inst.kind == Kind::DualTransfer && inst.rs == inst.rn))))
                return {};
            delta = inst.add ? inst.immediate : 0U - inst.immediate;
        } else if (inst.kind == Kind::MultipleTransfer) {
            if (inst.load && (inst.registers & (1U << inst.rn)) != 0)
                return {};
            const auto length = 4U * static_cast<unsigned>(std::popcount(inst.registers));
            delta = inst.add ? length : 0U - length;
        } else if (inst.kind == Kind::VectorTransfer) {
            const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
            if (v.mode == arm::VectorTransferMode::ScalarMultiple)
                delta = inst.add ? inst.immediate : 0U - inst.immediate;
            else if (inst.rm == 13 || inst.rm == 15)
                delta = arm_memory::vector_transfer_length(v);
            else
                return {};
        } else
            return {};
        return Change { inst.rn, std::bit_cast<std::int32_t>(delta) };
    }
    std::optional<std::int32_t> get(unsigned reg) const
    {
        const auto value = values_[reg];
        return value && !value->index
            ? std::make_optional(value->constant) : std::nullopt;
    }
    std::optional<Expression> expression(unsigned reg) const { return values_[reg]; }
    bool invariant(unsigned reg) const { return (written_ & (1U << reg)) == 0; }
    bool modified(unsigned reg) const { return (modified_ & (1U << reg)) != 0; }
    void observe(const arm::Instruction& inst)
    {
        auto change = writeback(inst);
        if (inst.kind == arm::InstructionKind::DataProcessing &&
            inst.immediate_operand && inst.rd == inst.rn && inst.rd < 15 &&
            (inst.opcode == 2 || inst.opcode == 4))
            change = Change { inst.rd, std::bit_cast<std::int32_t>(
                inst.opcode == 4 ? inst.immediate : 0U - inst.immediate) };
        std::optional<Expression> increment;
        if (change)
            increment = Expression { change->delta, 0, {} };
        if (inst.kind == arm::InstructionKind::DataProcessing &&
            !inst.immediate_operand && !inst.register_shift &&
            inst.shift == arm::ShiftKind::Lsl && inst.shift_amount < 32 &&
            inst.rd == inst.rn && inst.rd < 15 && inst.rm < 15 &&
            inst.rm != inst.rd && invariant(inst.rm) &&
            (inst.opcode == 2 || inst.opcode == 4)) {
            auto scale = std::uint32_t { 1 } << inst.shift_amount;
            if (inst.opcode == 2)
                scale = 0U - scale;
            change = Change { inst.rd, 0 };
            increment = Expression { 0, std::bit_cast<std::int32_t>(scale), inst.rm };
        }
        if (inst.condition != 14)
            change.reset();
        const auto before = change ? expression(change->reg) : std::nullopt;
        for_written_register(inst, [&](unsigned reg) {
            values_[reg].reset();
            written_ |= 1U << reg;
        });
        if (before) {
            modified_ |= 1U << change->reg;
            values_[change->reg] = before->added(*increment);
        }
    }
private:
    std::array<std::optional<Expression>, 16> values_;
    unsigned modified_ = 0, written_ = 0;
};
}
