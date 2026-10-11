/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "integer_emitter.hpp"
#include "arm/pc_write.hpp"
#include "arm/integer_semantics.hpp"
#include <optional>
#include <vector>

namespace ilemu::execution::arm64 {
// Compile-scoped target guards for exchanges and ALU PC writes. Matching
// targets stay in the trace; mismatches reuse architectural paths out of line.
class ExchangePaths {
public:
    struct Fallback {
        oaknut::Label label;
        arm::Instruction instruction;
        std::uint32_t pc, word;
        std::uint64_t before, ticks;
        unsigned it, next_it;
        bool interworks;
    };
    ExchangePaths(oaknut::VectorCodeGenerator& code, IntegerEmitter& integer,
        bool thumb)
        : code_(code)
        , integer_(integer)
        , thumb_(thumb)
    {
    }
    void emit(const arm::Instruction& inst, std::uint32_t pc,
        std::uint32_t word, std::uint64_t before, std::uint64_t ticks,
        unsigned it, unsigned next_it, std::uint32_t entry, bool interworks)
    {
        using namespace oaknut;
        using namespace oaknut::util;
        const auto pc_value =
            (pc + inst.pc_offset) & (inst.align_pc ? ~3U : ~0U);
        const bool alu = arm::alu_writes_pc(inst);
        const bool first_input = alu && inst.opcode != 13 && inst.opcode != 15;
        const bool second_input = !alu || !inst.immediate_operand;
        const bool reads_pc = (first_input && inst.rn == 15) ||
                              (second_input && inst.rm == 15);
        const Target key { word, reads_pc ? pc_value : 0U, entry, interworks };
        const auto input_mask =
            (first_input && inst.rn != 15 ? 1U << inst.rn : 0U) |
            (second_input && inst.rm != 15 ? 1U << inst.rm : 0U);
        // ADC/SBC/RSC and RRX also consume live carry. Keep their guards
        // even when their core-register operands are invariant.
        const bool carry_input = alu &&
            ((inst.opcode >= 5 && inst.opcode <= 7) ||
                (!inst.immediate_operand && inst.shift == arm::ShiftKind::Ror &&
                    inst.shift_amount == 0));
        invariant_ = (written_ & input_mask) == 0 && !carry_input;
        if (alu && input_mask == 0 && !carry_input) {
            // Reuse the portable ALU semantics for compile-time PC operands.
            // A proven constant backedge needs no runtime target guard.
            const auto operand = inst.immediate_operand ? inst.immediate
                : arm::shift(pc_value, inst.shift, inst.shift_amount, false, false).value;
            const auto raw = arm::calculate_alu(inst.opcode, false,
                first_input ? pc_value : 0U, { operand, false }, 0).value;
            const auto target = arm::alu_write_pc(raw, thumb_, interworks
                ? AluPcWriteProfile::Interworking : AluPcWriteProfile::Branch);
            if (target && target->address == entry && target->thumb == thumb_)
                return;
        }
        if (!first_ || *first_ != key || !invariant_) {
            pending_.push_back(
                { { }, inst, pc, word, before, ticks, it, next_it, interworks });
            const auto target = alu ? W15 : integer_.reg(inst.rm, pc_value, W17);
            if (alu)
                integer_.alu(inst, pc);
            // BranchWritePC ignores alignment bits without changing ISA.
            if (!interworks)
                code_.AND(W17, target, thumb_ ? ~1U : ~3U);
            code_.MOV(W16, entry | (interworks && thumb_ ? 1U : 0U));
            code_.EOR(W16, interworks ? target : W17, W16);
            code_.CBNZ(W16, pending_.back().label);
            if (!first_)
                first_ = key;
        }
        // The raw target proves both legality and unchanged PC/ISA. Read it
        // before writing LR, including BLX LR, and preserve guest NZCV.
        if (inst.link)
            code_.MOV(W14, (pc + inst.size) | (thumb_ ? 1U : 0U));
    }
    // Written inputs revoke reuse even when conditional or later restored.
    // The first guard still executes each native group and after every lease.
    void invalidate(unsigned reg) { written_ |= 1U << reg; }
    void invalidate_all() { written_ = 0xffffU; }
    auto& pending() { return pending_; }
    bool invariant() const { return invariant_; }

private:
    struct Target {
        std::uint32_t word, pc_value, entry;
        bool interworks;
        bool operator==(const Target&) const = default;
    };
    oaknut::VectorCodeGenerator& code_;
    IntegerEmitter& integer_;
    bool thumb_;
    unsigned written_ = 0;
    bool invariant_ = false;
    std::optional<Target> first_;
    std::vector<Fallback> pending_;
};
}
