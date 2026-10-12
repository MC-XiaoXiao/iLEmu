/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/run.hpp"
#include <deque>
#include <memory_resource>

namespace ilemu::execution::arm64 {
struct DecodedInstruction {
    std::uint32_t pc;
    unsigned it;
    std::optional<std::uint32_t> word;
    arm::Instruction instruction;
    std::optional<std::uint64_t> ticks;
};
// Compile-scoped inspection cache. Lookahead and emission share each decode;
// no guest state, backing pointer or metadata survives in an executor/cache.
class InstructionWindow {
public:
    InstructionWindow(InstructionSource& source, InstructionRegion region,
        bool thumb, std::pmr::memory_resource* temporary);
    std::optional<DecodedInstruction> take(std::uint32_t pc, unsigned it,
        bool allow_straddle);
    std::uint64_t timing(DecodedInstruction&);
    // A conservative linear-cycle proof avoids serializing parallel read
    // chains through one page-cache slot. Prefetched decodes feed emission.
    bool single_dependent_read_cycle(std::uint32_t pc, unsigned it,
        unsigned remaining_instructions, std::uint64_t remaining_ticks);
    bool flags_dead(const arm::Instruction&, std::uint32_t next_pc,
        unsigned next_it, unsigned remaining_instructions,
        std::uint64_t remaining_ticks);
    // A host clobber may cross comparisons whose trap controls were checked
    // clear on the executing path. FPSCR writes revoke that guarantee.
    bool flags_dead(unsigned mask, std::uint32_t next_pc, unsigned next_it,
        unsigned remaining_instructions, std::uint64_t remaining_ticks,
        std::uint32_t guarded_controls);

private:
    enum class FlagProofKind { Overwrite, TerminalFailure, BoundedFailure };
    struct FlagProof {
        unsigned reusable_prefix, instructions;
        std::uint64_t ticks;
        FlagProofKind kind;
    };
    std::optional<DecodedInstruction> decode(std::uint64_t pc, unsigned it,
        bool allow_straddle);
    void match(std::uint32_t pc, unsigned it);
    InstructionSource& source_;
    InstructionRegion region_;
    bool thumb_;
    ThumbArithmeticProfile arithmetic_;
    VfpRegisterProfile vfp_;
    VfpInstructionProfile floating_instructions_;
    ArmInstructionProfile architecture_;
    IntegerDivideProfile division_;
    std::pmr::deque<DecodedInstruction> pending_;
    std::optional<FlagProof> comparison_flags_dead_;
};
}
