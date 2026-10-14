/* SPDX-License-Identifier: MPL-2.0 */
#include "instruction_window.hpp"
#include "address_windows.hpp"
#include "multiple_emitter.hpp"
#include "scalar_register_plan.hpp"
#include "vector_memory_emitter.hpp"
#include "flag_effects.hpp"
#include "dynamic_backedge.hpp"
#include "memory_emitter.hpp"
#include "arm/a32_decode.hpp"
#include "arm/architectural_stop.hpp"
#include "arm/t32_decode.hpp"
#include "arm/floating_comparison.hpp"
#include <bit>
#include <array>

namespace ilemu::execution::arm64 {
InstructionWindow::InstructionWindow(InstructionSource& source,
    InstructionRegion region, bool thumb, std::pmr::memory_resource* temporary,
    bool retain_cycle)
    : source_(source), region_(region), thumb_(thumb)
    , arithmetic_(source.thumb_arithmetic_profile())
    , vfp_(source.vfp_register_profile())
    , floating_instructions_(source.vfp_instruction_profile())
    , architecture_(source.arm_instruction_profile())
    , division_(source.integer_divide_profile())
    , memory_(source.thumb_memory_profile())
    , simd_(source.advanced_simd_profile())
    , pending_(temporary)
    , retain_cycle_(retain_cycle)
{
}
std::optional<DecodedInstruction> InstructionWindow::decode(
    std::uint64_t pc, unsigned it, bool allow_straddle)
{
    if (pc < region_.begin || pc >= region_.end)
        return { };
    DecodedInstruction out { static_cast<std::uint32_t>(pc), it, { }, { }, { } };
    if (thumb_) {
        const auto first = source_.fetch16(out.pc);
        std::optional<std::uint16_t> second;
        if (first && arm::thumb_is_wide(*first)) {
            if (!allow_straddle && pc + 4U > region_.end)
                return { };
            second = source_.fetch16(out.pc + 2U);
        }
        if (first && (!arm::thumb_is_wide(*first) || second)) {
            out.word = *first | (std::uint32_t { second.value_or(0) } << 16U);
            out.instruction = arm::decode_t32(*first, second, it,
                arithmetic_, vfp_, architecture_, floating_instructions_,
                division_, memory_, simd_);
        }
    } else {
        out.word = source_.fetch32(out.pc);
        if (out.word)
            out.instruction = arm::decode_a32(*out.word, vfp_, architecture_,
                floating_instructions_, division_, simd_);
    }
    return out;
}
void InstructionWindow::match(std::uint32_t pc, unsigned it)
{
    if (consumed_ < pending_.size() &&
        (pending_[consumed_].pc != pc || pending_[consumed_].it != it)) {
        pending_.clear();
        consumed_ = 0;
        retain_cycle_ = false;
        comparison_flags_dead_.reset();
    }
}
std::optional<DecodedInstruction> InstructionWindow::take(
    std::uint32_t pc, unsigned it, bool allow_straddle)
{
    match(pc, it);
    if (consumed_ == pending_.size()) {
        auto out = decode(pc, it, allow_straddle);
        if (retain_cycle_ && out) {
            pending_.push_back(*out);
            ++consumed_;
        }
        return out;
    }
    auto out = pending_[consumed_];
    if (retain_cycle_)
        ++consumed_;
    else
        pending_.pop_front();
    if (comparison_flags_dead_) {
        if (--comparison_flags_dead_->reusable_prefix == 0)
            comparison_flags_dead_.reset();
        else if (comparison_flags_dead_->kind != FlagProofKind::TerminalFailure) {
            if (comparison_flags_dead_->instructions != 0)
                --comparison_flags_dead_->instructions;
            comparison_flags_dead_->ticks -= *out.ticks;
        }
    }
    return out;
}
std::uint64_t InstructionWindow::timing(DecodedInstruction& entry)
{
    if (!entry.ticks)
        entry.ticks = source_.ticks_for_instruction(entry.pc, *entry.word);
    // take() returns a copy; retain its paid timing in the original decode.
    if (retain_cycle_ && consumed_ != 0) {
        auto& original = pending_[consumed_ - 1U];
        if (original.pc == entry.pc && original.it == entry.it)
            original.ticks = entry.ticks;
    }
    return *entry.ticks;
}
void InstructionWindow::repeat_cycle()
{
    if (!retain_cycle_ || consumed_ == 0)
        return;
    // Lookahead beyond the closing edge belongs to its fallthrough, not to
    // this repeated path. The original architecture decodes stay unchanged.
    pending_.resize(consumed_);
    consumed_ = 0;
    comparison_flags_dead_.reset();
}
ScalarRegisterPlan InstructionWindow::scalar_registers(std::uint32_t start,
    unsigned start_it, unsigned limit, std::uint64_t budget)
{
    ScalarRegisterPlan::Builder builder;
    auto pc = start;
    auto it = start_it;
    for (unsigned n = 0; n < limit; ++n) {
        auto entry = take(pc, it, false);
        if (!entry || !entry->word || entry->instruction.condition == 15 ||
            std::uint64_t { pc } + entry->instruction.size > region_.end)
            return {};
        const auto& inst = entry->instruction;
        const auto cost = timing(*entry);
        if (cost == 0 || cost > budget)
            return {};
        budget -= cost;
        const auto next_it = thumb_ ? (inst.kind == arm::InstructionKind::IfThen
            ? inst.immediate : arm::advance_it(it)) : 0U;
        using Kind = arm::InstructionKind;
        if (inst.kind == Kind::Branch) {
            const auto target = ((pc + inst.pc_offset) &
                (inst.align_pc ? ~3U : ~0U)) + inst.immediate;
            return !inst.exchange && target == start && next_it == start_it
                ? builder.finish() : ScalarRegisterPlan {};
        }
        if (inst.kind == Kind::FloatingArithmetic)
            builder.observe(std::get<arm::FloatingArithmeticOperands>(inst.vector));
        else if (inst.kind == Kind::VectorTransfer)
            builder.observe(std::get<arm::VectorTransferOperands>(inst.vector), inst.load);
        else {
            // Other extension consumers/producers retain their existing path.
            switch (inst.kind) {
            case Kind::DataProcessing:
                if (arm::alu_writes_pc(inst))
                    return {};
                break;
            case Kind::Transfer:
            case Kind::MultipleTransfer:
                if (DynamicBackedge::ends_trace(inst))
                    return {};
                break;
            case Kind::DualTransfer:
            case Kind::ExclusiveTransfer:
            case Kind::ClearExclusive:
            case Kind::Swap:
            case Kind::Multiply:
            case Kind::IntegerDivide:
            case Kind::PackHalfword:
            case Kind::IntegerTransform:
            case Kind::ParallelSaturating:
            case Kind::ParallelArithmetic:
            case Kind::SaturatingArithmetic:
            case Kind::WideImmediate:
            case Kind::Nop:
            case Kind::IfThen:
                break;
            default:
                return {};
            }
        }
        pc += inst.size;
        it = next_it;
    }
    return {};
}
AddressWindows InstructionWindow::address_windows(std::uint32_t start,
    unsigned start_it, unsigned limit, std::uint64_t budget)
{
    auto* resource = pending_.get_allocator().resource();
    AddressWindows result(resource);
    const auto slots = single_dependent_read_cycle(start, start_it, limit, budget)
        ? 2U : 4U;
    auto pc = start;
    auto it = start_it;
    for (unsigned n = 0; n < limit; ++n) {
        auto entry = take(pc, it, false);
        if (!entry || !entry->word || entry->instruction.condition == 15 ||
            std::uint64_t { pc } + entry->instruction.size > region_.end)
            return AddressWindows(resource);
        const auto& inst = entry->instruction;
        const auto cost = timing(*entry);
        if (cost == 0 || cost > budget)
            return AddressWindows(resource);
        budget -= cost;
        const auto next_it = thumb_ ? (inst.kind == arm::InstructionKind::IfThen
            ? inst.immediate : arm::advance_it(it)) : 0U;
        if (inst.kind == arm::InstructionKind::Branch) {
            const auto target = ((pc + inst.pc_offset) &
                (inst.align_pc ? ~3U : ~0U)) + inst.immediate;
            if (inst.exchange || target != start || next_it != start_it)
                return AddressWindows(resource);
            result.observe(inst, {});
            result.finish(slots);
            return result;
        }
        if (DynamicBackedge::ends_trace(inst) || arm::ArchitecturalStop::from(inst) ||
            inst.kind == arm::InstructionKind::Unsupported ||
            inst.kind == arm::InstructionKind::ExclusiveTransfer ||
            inst.kind == arm::InstructionKind::Swap ||
            inst.kind == arm::InstructionKind::ClearExclusive ||
            inst.kind == arm::InstructionKind::SystemControl ||
            inst.kind == arm::InstructionKind::CompareBranch)
            return AddressWindows(resource);
        std::optional<AddressCache::Key> key;
        if (inst.kind == arm::InstructionKind::Transfer ||
            inst.kind == arm::InstructionKind::DualTransfer)
            key = MemoryEmitter::address(inst, pc);
        else if (inst.kind == arm::InstructionKind::MultipleTransfer)
            key = MultipleEmitter::address(inst);
        else if (inst.kind == arm::InstructionKind::VectorTransfer &&
            !inst.writeback && inst.rn != 15)
            key = VectorMemoryEmitter::address(inst);
        result.observe(inst, key);
        pc += inst.size;
        it = next_it;
    }
    return AddressWindows(resource);
}
bool InstructionWindow::single_dependent_read_cycle(std::uint32_t start,
    unsigned start_it, unsigned remaining_instructions,
    std::uint64_t remaining_ticks)
{
    match(start, start_it);
    std::optional<unsigned> chain;
    std::uint64_t pc = start;
    auto it = start_it;
    for (unsigned i = 0; i < remaining_instructions; ++i) {
        if (consumed_ + i == pending_.size()) {
            auto entry = decode(pc, it, false);
            if (!entry)
                return false;
            pending_.push_back(*entry);
        }
        auto& entry = pending_[consumed_ + i];
        const auto& inst = entry.instruction;
        if (!entry.word || pc + inst.size > region_.end || inst.condition == 15)
            return false;
        const auto cost = timing(entry);
        if (cost == 0 || cost > remaining_ticks)
            return false;
        remaining_ticks -= cost;
        if (const auto destination =
                MemoryEmitter::dependent_read_destination(inst, entry.pc)) {
            if (chain && chain != destination)
                return false;
            chain = destination;
        }
        it = thumb_ ? (inst.kind == arm::InstructionKind::IfThen
                          ? inst.immediate : arm::advance_it(it)) : 0U;
        if (inst.kind == arm::InstructionKind::Branch) {
            const auto target =
                ((entry.pc + inst.pc_offset) & (inst.align_pc ? ~3U : ~0U)) +
                inst.immediate;
            return !inst.exchange && target == start && it == start_it &&
                chain.has_value();
        }
        if (inst.kind == arm::InstructionKind::CompareBranch ||
            DynamicBackedge::ends_trace(inst) ||
            arm::ArchitecturalStop::from(inst))
            return false;
        pc += inst.size;
    }
    return false;
}
std::optional<InvariantShiftCount> InstructionWindow::invariant_shift_count(
    std::uint32_t start, unsigned start_it, unsigned limit, std::uint64_t budget,
    bool include_carry)
{
    if (!retain_cycle_)
        return {};
    auto pc = start;
    auto it = start_it;
    unsigned candidates = 0, written = 0;
    std::array<unsigned, 15> carry_kinds {};
    std::array<unsigned, 15> kinds {};
    for (unsigned n = 0; n < limit; ++n) {
        auto entry = take(pc, it, false);
        if (!entry || !entry->word || entry->instruction.condition == 15 ||
            std::uint64_t { pc } + entry->instruction.size > region_.end)
            return {};
        const auto& inst = entry->instruction;
        const auto cost = timing(*entry);
        if (cost == 0 || cost > budget)
            return {};
        budget -= cost;
        const auto next_it = thumb_ ? arm::advance_it(it) : 0U;
        if (inst.kind == arm::InstructionKind::Branch) {
            if (inst.link)
                written |= 1U << 14U;
            const auto target =
                ((pc + inst.pc_offset) & (inst.align_pc ? ~3U : ~0U)) + inst.immediate;
            if (inst.exchange || target != start || next_it != start_it)
                return {};
            candidates &= ~written;
            if (candidates == 0)
                return {};
            const auto reg = static_cast<unsigned>(std::countr_zero(candidates));
            return InvariantShiftCount { reg, carry_kinds[reg], kinds[reg] };
        }
        // Memory, indirect control and other effects remain on their existing
        // paths. Conditional ALU writes also revoke invariance conservatively.
        if (inst.kind != arm::InstructionKind::DataProcessing || arm::alu_writes_pc(inst))
            return {};
        if (inst.opcode < 8 || inst.opcode > 11)
            written |= 1U << inst.rd;
        const bool logical = inst.opcode == 0 || inst.opcode == 1 ||
            inst.opcode == 8 || inst.opcode == 9 || inst.opcode >= 12;
        if (inst.register_shift && inst.rs < 15) {
            kinds[inst.rs] |= 1U << static_cast<unsigned>(inst.shift);
            const bool carry = logical && inst.set_flags &&
                !flags_dead(inst, pc + inst.size, next_it, limit - n - 1U, budget);
            if ((!carry || include_carry) &&
                (inst.shift != arm::ShiftKind::Ror || carry)) {
                candidates |= 1U << inst.rs;
                if (carry)
                    carry_kinds[inst.rs] |= 1U << static_cast<unsigned>(inst.shift);
            }
        }
        pc += inst.size;
        it = next_it;
    }
    return {};
}
bool InstructionWindow::flags_dead(const arm::Instruction& inst,
    std::uint32_t next_pc, unsigned next_it, unsigned remaining_instructions,
    std::uint64_t remaining_ticks)
{
    const auto producer = flag_effects(inst);
    if (producer.possible_write == 0 || producer.boundary)
        return false;
    return flags_dead(producer.possible_write, next_pc, next_it,
        remaining_instructions, remaining_ticks, 0);
}
bool InstructionWindow::flags_dead(unsigned live, std::uint32_t next_pc,
    unsigned next_it, unsigned remaining_instructions,
    std::uint64_t remaining_ticks, std::uint32_t guarded_controls)
{
    if (live == 0)
        return false;
    match(next_pc, next_it);
    const bool reusable = live == 0xfU &&
        guarded_controls == arm::floating_comparison_controls;
    if (reusable && comparison_flags_dead_) {
        const auto& proof = *comparison_flags_dead_;
        if (proof.kind == FlagProofKind::TerminalFailure)
            return false;
        if (proof.kind == FlagProofKind::Overwrite)
            return proof.instructions <= remaining_instructions &&
                proof.ticks <= remaining_ticks;
        // An exhausted search only excludes overwrites inside its covered
        // instruction or tick boundary. Larger queries must inspect again.
        if (remaining_instructions <= proof.instructions ||
            remaining_ticks <= proof.ticks)
            return false;
        comparison_flags_dead_.reset();
    }
    const auto initial_ticks = remaining_ticks;
    unsigned reusable_prefix = 0;
    std::uint64_t pc = next_pc;
    auto it = next_it;
    const auto terminal_failure = [&] {
        if (reusable_prefix != 0)
            comparison_flags_dead_ = FlagProof { reusable_prefix, 0, 0,
                FlagProofKind::TerminalFailure };
        return false;
    };
    const auto bounded_failure = [&](unsigned instructions, std::uint64_t ticks) {
        if (reusable_prefix != 0)
            comparison_flags_dead_ = FlagProof { reusable_prefix, instructions,
                ticks, FlagProofKind::BoundedFailure };
        return false;
    };
    for (unsigned i = 0; i < remaining_instructions; ++i) {
        if (consumed_ + i == pending_.size()) {
            auto entry = decode(pc, it, false);
            if (!entry)
                return terminal_failure();
            pending_.push_back(*entry);
        }
        auto& entry = pending_[consumed_ + i];
        if (reusable && live == 0xfU &&
            guarded_controls == arm::floating_comparison_controls)
            reusable_prefix = i + 1U;
        if (!entry.word || pc + entry.instruction.size > region_.end)
            return terminal_failure();
        auto effects = flag_effects(entry.instruction);
        if (entry.instruction.kind == arm::InstructionKind::FloatingComparison &&
            entry.instruction.condition != 15 &&
            (guarded_controls & arm::floating_comparison_controls) ==
                arm::floating_comparison_controls)
            effects.boundary = false;
        if (effects.boundary || (effects.read & live) != 0)
            return terminal_failure();
        const auto cost = timing(entry);
        if (cost == 0)
            return terminal_failure();
        if (cost > remaining_ticks)
            return bounded_failure(i, initial_ticks);
        remaining_ticks -= cost;
        live &= ~effects.guaranteed_write;
        if (live == 0) {
            // Transparent prefixes share the same overwrite proof. Keeping
            // its paid length avoids rescanning every suffix of a long run
            // of comparisons, without relaxing either compilation limit.
            if (reusable_prefix != 0)
                comparison_flags_dead_ = FlagProof { reusable_prefix,
                    i + 1U, initial_ticks - remaining_ticks,
                    FlagProofKind::Overwrite };
            return true;
        }
        if (entry.instruction.kind == arm::InstructionKind::FloatingSystem &&
            !std::get<arm::FloatingSystemOperands>(entry.instruction.vector).to_core)
            guarded_controls = 0;
        pc += entry.instruction.size;
        it = thumb_ ? (entry.instruction.kind == arm::InstructionKind::IfThen
                          ? entry.instruction.immediate : arm::advance_it(it))
                    : 0U;
    }
    return bounded_failure(
        remaining_instructions, initial_ticks - remaining_ticks);
}
}
