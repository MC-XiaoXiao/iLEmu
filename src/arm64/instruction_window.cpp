/* SPDX-License-Identifier: MPL-2.0 */
#include "instruction_window.hpp"
#include "flag_effects.hpp"
#include "arm/a32_decode.hpp"
#include "arm/t32_decode.hpp"
#include "arm/floating_comparison.hpp"

namespace ilemu::execution::arm64 {
InstructionWindow::InstructionWindow(InstructionSource& source,
    InstructionRegion region, bool thumb, std::pmr::memory_resource* temporary)
    : source_(source), region_(region), thumb_(thumb)
    , arithmetic_(source.thumb_arithmetic_profile())
    , vfp_(source.vfp_register_profile())
    , floating_instructions_(source.vfp_instruction_profile())
    , architecture_(source.arm_instruction_profile())
    , pending_(temporary)
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
                arithmetic_, vfp_, architecture_, floating_instructions_);
        }
    } else {
        out.word = source_.fetch32(out.pc);
        if (out.word)
            out.instruction = arm::decode_a32(*out.word, vfp_, architecture_, floating_instructions_);
    }
    return out;
}
void InstructionWindow::match(std::uint32_t pc, unsigned it)
{
    if (!pending_.empty() &&
        (pending_.front().pc != pc || pending_.front().it != it)) {
        pending_.clear();
        comparison_flags_dead_.reset();
    }
}
std::optional<DecodedInstruction> InstructionWindow::take(
    std::uint32_t pc, unsigned it, bool allow_straddle)
{
    match(pc, it);
    if (pending_.empty())
        return decode(pc, it, allow_straddle);
    auto out = pending_.front();
    pending_.pop_front();
    if (comparison_flags_dead_) {
        if (--comparison_flags_dead_->reusable_prefix == 0)
            comparison_flags_dead_.reset();
        else if (comparison_flags_dead_->instructions != 0) {
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
    return *entry.ticks;
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
        return proof.instructions != 0 &&
            proof.instructions <= remaining_instructions &&
            proof.ticks <= remaining_ticks;
    }
    const auto initial_ticks = remaining_ticks;
    unsigned reusable_prefix = 0;
    std::uint64_t pc = next_pc;
    auto it = next_it;
    const auto terminal_failure = [&] {
        if (reusable_prefix != 0)
            comparison_flags_dead_ = FlagProof { reusable_prefix, 0, 0 };
        return false;
    };
    for (unsigned i = 0; i < remaining_instructions; ++i) {
        if (i == pending_.size()) {
            auto entry = decode(pc, it, false);
            if (!entry)
                return terminal_failure();
            pending_.push_back(*entry);
        }
        auto& entry = pending_[i];
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
            return false;
        remaining_ticks -= cost;
        live &= ~effects.guaranteed_write;
        if (live == 0) {
            // Transparent prefixes share the same overwrite proof. Keeping
            // its paid length avoids rescanning every suffix of a long run
            // of comparisons, without relaxing either compilation limit.
            if (reusable_prefix != 0)
                comparison_flags_dead_ = FlagProof { reusable_prefix,
                    i + 1U, initial_ticks - remaining_ticks };
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
    return false;
}
}
