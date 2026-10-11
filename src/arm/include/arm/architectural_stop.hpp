/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "arm/t32_decode.hpp"
#include "execution/run.hpp"

namespace ilemu::arm {
// Retirement shared by portable execution and standalone native stop entries.
// Callers validate conditions, fetches and timing before retiring the stop.
class ArchitecturalStop {
public:
    static std::optional<ArchitecturalStop> from(const Instruction& instruction)
    {
        if (instruction.kind == InstructionKind::Svc)
            return ArchitecturalStop { true,
                instruction.size, instruction.immediate };
        if (instruction.kind == InstructionKind::Breakpoint)
            return ArchitecturalStop { false,
                instruction.size, 0 };
        return { };
    }
    void retire(CpuThreadState& state, execution::RunResult& result,
        bool thumb, unsigned it, std::uint64_t ticks,
        execution::ExecutionMode mode) const
    {
        if (svc_) {
            state.registers[15] = result.pc + size_;
            if (thumb)
                state.cpsr = with_it_state(state.cpsr, advance_it(it));
            result.svc = immediate_;
        }
        result.ticks_consumed += ticks;
        result.reason = svc_ ? execution::StopReason::Svc
                             : execution::StopReason::Breakpoint;
        if (mode == execution::ExecutionMode::SingleStep)
            result.reason = result.reason | execution::StopReason::SingleStep;
    }

private:
    ArchitecturalStop(bool svc, unsigned size,
        std::uint32_t immediate)
        : immediate_(immediate), size_(static_cast<std::uint8_t>(size)), svc_(svc)
    {
    }
    std::uint32_t immediate_;
    std::uint8_t size_;
    bool svc_;
};
}
