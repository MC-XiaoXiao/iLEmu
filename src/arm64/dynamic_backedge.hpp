/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/run.hpp"
#include "arm/pc_write.hpp"
#include <oaknut/oaknut.hpp>

namespace ilemu::execution::arm64 {
// A dynamic target can reuse this trace only after a complete, equal-cost
// iteration. No target, code pointer or memory value is cached across leases.
class DynamicBackedge {
public:
    DynamicBackedge(std::uint32_t pc, unsigned it, bool thumb, bool enabled)
        : pc_(pc), it_(it), thumb_(thumb), enabled_(enabled)
    {
    }
    static bool ends_trace(const arm::Instruction& inst)
    {
        using arm::InstructionKind;
        switch (inst.kind) {
        case InstructionKind::BranchExchange:
        case InstructionKind::TableBranch:
            return true;
        case InstructionKind::Transfer:
            return inst.load && inst.rd == 15;
        case InstructionKind::MultipleTransfer:
            return inst.load && (inst.registers & 0x8000U) != 0;
        case InstructionKind::DataProcessing:
            return arm::alu_writes_pc(inst);
        default:
            return false;
        }
    }
    bool accepts(std::optional<StopReason> reason, std::uint64_t ticks,
        bool dynamic_pc, unsigned it, std::uint64_t maximum_ticks) const
    {
        return enabled_ && dynamic_pc && reason == StopReason::None &&
               ticks == maximum_ticks && it == it_;
    }
    void branch_on_mismatch(
        oaknut::VectorCodeGenerator& code, oaknut::Label& mismatch) const
    {
        using namespace oaknut;
        using namespace oaknut::util;
        if (thumb_)
            code.TBZ(W22, 5, mismatch);
        else
            code.TBNZ(W22, 5, mismatch);
        // Preserve guest NZCV; a comparison must not become guest flags.
        code.MOV(W16, pc_);
        code.EOR(W16, W15, W16);
        code.CBNZ(W16, mismatch);
    }
    void emit(oaknut::VectorCodeGenerator& code, oaknut::Label& tail) const
    {
        oaknut::Label mismatch;
        branch_on_mismatch(code, mismatch);
        code.B(tail);
        code.l(mismatch);
    }
    void disable() { enabled_ = false; }
    std::uint32_t pc() const { return pc_; }
    unsigned it() const { return it_; }

private:
    std::uint32_t pc_;
    unsigned it_;
    bool thumb_, enabled_;
};
}
