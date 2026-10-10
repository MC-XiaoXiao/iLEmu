/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/run.hpp"
#include <deque>

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
        bool thumb);
    std::optional<DecodedInstruction> take(std::uint32_t pc, unsigned it,
        bool allow_straddle);
    std::uint64_t timing(DecodedInstruction&);
    bool flags_dead(const arm::Instruction&, std::uint32_t next_pc,
        unsigned next_it, unsigned remaining_instructions,
        std::uint64_t remaining_ticks);

private:
    std::optional<DecodedInstruction> decode(std::uint64_t pc, unsigned it,
        bool allow_straddle);
    void match(std::uint32_t pc, unsigned it);
    InstructionSource& source_;
    InstructionRegion region_;
    bool thumb_;
    ThumbArithmeticProfile arithmetic_;
    VfpRegisterProfile vfp_;
    ArmInstructionProfile architecture_;
    std::deque<DecodedInstruction> pending_;
};
}
