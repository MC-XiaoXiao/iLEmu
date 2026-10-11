/* SPDX-License-Identifier: MPL-2.0 */
#include "instruction_window.hpp"
#include "flag_effects.hpp"
#include "arm/a32_decode.hpp"
#include "arm/t32_decode.hpp"

namespace ilemu::execution::arm64 {
InstructionWindow::InstructionWindow(InstructionSource& source,
    InstructionRegion region, bool thumb)
    : source_(source), region_(region), thumb_(thumb)
    , arithmetic_(source.thumb_arithmetic_profile())
    , vfp_(source.vfp_register_profile())
    , floating_instructions_(source.vfp_instruction_profile())
    , architecture_(source.arm_instruction_profile())
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
        (pending_.front().pc != pc || pending_.front().it != it))
        pending_.clear();
}
std::optional<DecodedInstruction> InstructionWindow::take(
    std::uint32_t pc, unsigned it, bool allow_straddle)
{
    match(pc, it);
    if (pending_.empty())
        return decode(pc, it, allow_straddle);
    auto out = pending_.front();
    pending_.pop_front();
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
    auto live = producer.possible_write;
    if (live == 0 || producer.boundary)
        return false;
    match(next_pc, next_it);
    std::uint64_t pc = next_pc;
    auto it = next_it;
    for (unsigned i = 0; i < remaining_instructions; ++i) {
        if (i == pending_.size()) {
            auto entry = decode(pc, it, false);
            if (!entry)
                return false;
            pending_.push_back(*entry);
        }
        auto& entry = pending_[i];
        if (!entry.word || pc + entry.instruction.size > region_.end)
            return false;
        const auto effects = flag_effects(entry.instruction);
        if (effects.boundary || (effects.read & live) != 0)
            return false;
        const auto cost = timing(entry);
        if (cost == 0 || cost > remaining_ticks)
            return false;
        remaining_ticks -= cost;
        live &= ~effects.guaranteed_write;
        if (live == 0)
            return true;
        pc += entry.instruction.size;
        it = thumb_ ? (entry.instruction.kind == arm::InstructionKind::IfThen
                          ? entry.instruction.immediate : arm::advance_it(it))
                    : 0U;
    }
    return false;
}
}
