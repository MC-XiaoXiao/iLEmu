/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "code_regions.hpp"
#include "trace_path.hpp"
#include "execution/run.hpp"
#include <array>
#include <deque>
#include <memory_resource>

namespace ilemu::execution::arm64 {
class AddressWindows;
struct FloatingRegisterPlan;
struct DecodedInstruction {
    std::uint32_t pc;
    unsigned it;
    std::optional<std::uint32_t> word;
    arm::Instruction instruction;
    std::optional<std::uint64_t> ticks;
};
struct InvariantShiftCount {
    unsigned reg;
    unsigned carry_kinds;
    unsigned kinds;
};
// Compile-scoped inspection cache. Lookahead and emission share each decode;
// no guest state, backing pointer or metadata survives in an executor/cache.
class InstructionWindow {
public:
    InstructionWindow(InstructionSource& source, InstructionRegion region,
        bool thumb, std::pmr::memory_resource* temporary, bool retain_cycle = false);
    InstructionWindow(InstructionSource& source, CodeRegions regions,
        bool thumb, std::pmr::memory_resource* temporary, bool retain_cycle = false);
    void mode(bool thumb);
    void regions(CodeRegions regions);
    // Rewind only after the compiler accepts another copy of its entry cycle.
    void repeat_cycle();
    std::size_t cached_instructions() const noexcept { return pending_.size(); }
    std::optional<DecodedInstruction> take(std::uint32_t pc, unsigned it,
        bool allow_straddle);
    std::uint64_t timing(DecodedInstruction&);
    FloatingRegisterPlan floating_registers(std::uint32_t pc, unsigned it,
        unsigned remaining_instructions, std::uint64_t remaining_ticks,
        bool reserve_scalars = true, const TracePath& path = TracePath {});
    // Prove page-local windows for stable bases and bounded offsets.
    AddressWindows address_windows(std::uint32_t pc, unsigned it,
        unsigned remaining_instructions, std::uint64_t remaining_ticks,
        const TracePath& path = TracePath {});
    // A conservative linear-cycle proof avoids serializing parallel read
    // chains through one page-cache slot. Prefetched decodes feed emission.
    bool single_dependent_read_cycle(std::uint32_t pc, unsigned it,
        unsigned remaining_instructions, std::uint64_t remaining_ticks);
    // Consume a conservative ordinary ALU cycle. Only choose a count whose
    // value survives every possible write and whose range can improve lowering.
    std::optional<InvariantShiftCount> invariant_shift_count(std::uint32_t pc,
        unsigned it, unsigned remaining_instructions,
        std::uint64_t remaining_ticks, bool include_carry = true);
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
    CodeRegions regions_;
    bool thumb_;
    bool mixed_timing_;
    ThumbArithmeticProfile arithmetic_;
    VfpRegisterProfile vfp_;
    VfpInstructionProfile floating_instructions_;
    ArmInstructionProfile architecture_;
    IntegerDivideProfile division_;
    ThumbMemoryProfile memory_;
    AdvancedSimdProfile simd_;
    std::pmr::deque<DecodedInstruction> pending_;
    std::size_t consumed_ = 0;
    bool retain_cycle_;
    // Full-NZCV proofs differ when FP trap controls have been checked clear.
    // Both caches are compile-scoped and advance with the paid decode window.
    std::array<std::optional<FlagProof>, 2> full_flags_dead_;
};
}
