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
    AddressDisplacements()
    {
        for (auto& value : values_)
            value = 0;
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
    std::optional<std::int32_t> get(unsigned reg) const { return values_[reg]; }
    bool modified(unsigned reg) const { return (modified_ & (1U << reg)) != 0; }
    void observe(const arm::Instruction& inst)
    {
        auto change = writeback(inst);
        if (inst.kind == arm::InstructionKind::DataProcessing &&
            inst.immediate_operand && inst.rd == inst.rn && inst.rd < 15 &&
            (inst.opcode == 2 || inst.opcode == 4))
            change = Change { inst.rd, std::bit_cast<std::int32_t>(
                inst.opcode == 4 ? inst.immediate : 0U - inst.immediate) };
        if (inst.condition != 14)
            change.reset();
        const auto before = change ? get(change->reg) : std::nullopt;
        for_written_register(inst, [&](unsigned reg) { values_[reg].reset(); });
        if (before) {
            modified_ |= 1U << change->reg;
            values_[change->reg] = std::bit_cast<std::int32_t>(
                static_cast<std::uint32_t>(*before) +
                static_cast<std::uint32_t>(change->delta));
        }
    }
private:
    std::array<std::optional<std::int32_t>, 16> values_;
    unsigned modified_ = 0;
};
}
