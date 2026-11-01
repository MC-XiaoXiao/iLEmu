/* SPDX-License-Identifier: MPL-2.0 */
#include "foundation/jit_tiered_execution.hpp"
#include "arm_interpreter/interpreter.hpp"
#include "foundation/performance.hpp"
#include <algorithm>
#include <array>

namespace ilemu {
std::shared_ptr<HostWorkToken> JitTieredExecution::submit(
    HostResourceController::CancellableWork work)
{
    auto token = resources_.submit_cancellable(HostWorkKind::BackgroundCompile,
        std::nullopt, std::move(work), std::chrono::milliseconds{2});
    if (token)
        requests_.fetch_add(1, std::memory_order_relaxed);
    return token;
}
void JitTieredExecution::interpreted(std::uint64_t ticks,
    std::uint64_t nanoseconds, bool ready)
{
    ticks_.fetch_add(ticks, std::memory_order_relaxed);
    nanoseconds_.fetch_add(nanoseconds, std::memory_order_relaxed);
    ready_.fetch_add(ready, std::memory_order_relaxed);
}
JitTieredStatistics JitTieredExecution::statistics() const
{
    return {requests_.load(), compiled_.load(), ready_.load(), ticks_.load(),
        nanoseconds_.load(), fallbacks_.load()};
}
execution::RunResult TieredInterpreter::run(CpuThreadState& state,
    execution::InstructionSource& source, const execution::RunRequest& request)
{
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    // Reuse the compile-cost history already maintained for host admission.
    // Pass the local deadline into the interpreter: checking only between
    // 32-instruction batches let memory synchronization overrun the quantum.
    const auto quantum = std::chrono::nanoseconds{std::clamp<std::uint64_t>(
        performance_counters().jit_block_compile_p95_nanoseconds(), 25'000, 200'000)};
    auto bounded = request;
    bounded.deadline = std::min(request.deadline, started + quantum);
    execution::RunResult result;
    bool ready = false;
    std::array<std::uint32_t, 16> observed{};
    std::size_t observations = 0;
    for (;;) {
        ready = ready_(state);
        if (ready || Clock::now() >= bounded.deadline) {
            result.pc = state.registers[15];
            result.reason = execution::StopReason::BackendSwitch;
            break;
        }
        const auto count = std::min(observations, observed.size());
        if (std::find(observed.begin(), observed.begin()+static_cast<std::ptrdiff_t>(count),
                state.registers[15]) != observed.begin()+static_cast<std::ptrdiff_t>(count))
            prepare_(state);
        else
            observed[observations++ % observed.size()] = state.registers[15];
        result = execution::ArmInterpreter::run_slice(state, source, bounded,
            context_, 32, result.ticks_consumed);
        if (result.reason != execution::StopReason::None)
            break;
    }
    if (execution::has_reason(result.reason, execution::StopReason::HostDeadline) &&
        bounded.deadline < request.deadline && Clock::now() < request.deadline) {
        result.reason = static_cast<execution::StopReason>(
            static_cast<std::uint32_t>(result.reason) &
            ~static_cast<std::uint32_t>(execution::StopReason::HostDeadline));
        result.reason = result.reason | execution::StopReason::BackendSwitch;
    }
    service_->interpreted(result.ticks_consumed,
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now()-started).count()), ready);
    return result;
}
}
