/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/arm_state.hpp"
#include "execution/context.hpp"

namespace ilemu::execution {

// Stateless guest context binding permits reuse by a runtime execution slot.
// Run calls are serialized by its owner; request_stop is thread-safe.
class ArmInterpreter final : public Executor {
public:
    RunResult run(CpuThreadState& state, InstructionSource& source,
        const RunRequest& request) override;
    // Internal continuation: None means the instruction limit was reached.
    // Polls and the original tick/deadline boundary precede that yield. Ticks
    // include prior slices, preserving overflow checks across backend changes.
    static RunResult run_slice(CpuThreadState&, InstructionSource&,
        const RunRequest&, ExecutorContext&, std::uint64_t instruction_limit,
        std::uint64_t ticks_consumed = 0);

    void request_stop(StopReason reason) noexcept override
    {
        context_.stops.request(reason);
    }

    void clear_exclusive_state() override
    { context_.clear_exclusive_state(); }

    void clear_stop() noexcept override { static_cast<void>(context_.stops.consume()); }

private:
    ExecutorContext context_;
};

}
