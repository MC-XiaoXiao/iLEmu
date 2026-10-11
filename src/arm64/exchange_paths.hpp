/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "integer_emitter.hpp"
#include <optional>
#include <vector>

namespace ilemu::execution::arm64 {
// Compile-scoped raw-target guards. A matching exchange stays in the trace;
// every mismatch reuses the full architectural exchange path out of line.
class ExchangePaths {
public:
    struct Fallback {
        oaknut::Label label;
        arm::Instruction instruction;
        std::uint32_t pc, word;
        std::uint64_t before, ticks;
        unsigned it, next_it;
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
        unsigned it, unsigned next_it, std::uint32_t entry)
    {
        using namespace oaknut;
        using namespace oaknut::util;
        const auto pc_value =
            (pc + inst.pc_offset) & (inst.align_pc ? ~3U : ~0U);
        const Target key { inst.rm, inst.rm == 15 ? pc_value : 0U, entry };
        const bool invariant = inst.rm == 15 || (written_ & (1U << inst.rm)) == 0;
        if (!first_ || *first_ != key || !invariant) {
            pending_.push_back({ { }, inst, pc, word, before, ticks, it, next_it });
            const auto target = integer_.reg(inst.rm, pc_value, W17);
            code_.MOV(W16, entry | (thumb_ ? 1U : 0U));
            code_.EOR(W16, target, W16);
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

private:
    struct Target {
        unsigned reg;
        std::uint32_t pc_value, entry;
        bool operator==(const Target&) const = default;
    };
    oaknut::VectorCodeGenerator& code_;
    IntegerEmitter& integer_;
    bool thumb_;
    unsigned written_ = 0;
    std::optional<Target> first_;
    std::vector<Fallback> pending_;
};
}
