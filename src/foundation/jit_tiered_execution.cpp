/* SPDX-License-Identifier: MPL-2.0 */
#include "foundation/jit_tiered_execution.hpp"
#include "arm_interpreter/interpreter.hpp"
#include "foundation/performance.hpp"
#include <dynarmic/ir/basic_block.h>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <algorithm>
#include <mutex>

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
std::shared_ptr<HostWorkToken> JitTieredExecution::submit_demand(
    HostResourceController::CancellableWork work)
{
    auto token = resources_.submit_cancellable(HostWorkKind::ForegroundPrepare,
        std::nullopt, std::move(work));
    if (token) {
        requests_.fetch_add(1, std::memory_order_relaxed);
        demand_requests_.fetch_add(1, std::memory_order_relaxed);
    }
    return token;
}

struct JitEmissionQueue::Impl {
    struct Job {
        std::uint64_t descriptor;
        std::uint32_t end;
        std::shared_ptr<HostWorkToken> token;
    };
    std::shared_ptr<JitTieredExecution> service;
    Emit emit;
    mutable std::mutex mutex;
    std::shared_ptr<Job> job;
};
JitEmissionQueue::JitEmissionQueue(std::shared_ptr<JitTieredExecution> service, Emit emit)
    : impl_(std::make_unique<Impl>())
{
    impl_->service = std::move(service);
    impl_->emit = std::move(emit);
}
JitEmissionQueue::~JitEmissionQueue() { cancel(); }
bool JitEmissionQueue::defer(Dynarmic::IR::Block& block,
    std::uint64_t generation, std::uint64_t revision)
{
    // Small blocks cost less to emit than a worker handoff. Large blocks are
    // already bounded by the baseline frontend and transferred without decoding
    // them twice. Keep the immutable ticket and end PC outside the moved IR.
    if (block.size() <= 256)
        return false;
    const std::lock_guard lock{impl_->mutex};
    if (impl_->job && !impl_->job->token->finished())
        return false;
    auto job = std::make_shared<Impl::Job>(block.Location().Value(),
        Dynarmic::A32::LocationDescriptor{block.EndLocation()}.PC());
    auto prepared = std::make_shared<Dynarmic::IR::Block>(std::move(block));
    job->token = impl_->service->submit_demand(
        [this, prepared, generation, revision](const HostWorkToken& token) {
            if (!token.cancelled() && impl_->emit(std::move(*prepared), generation, revision))
                impl_->service->compiled(1);
        });
    if (!job->token) {
        block = std::move(*prepared);
        return false;
    }
    impl_->job = std::move(job);
    return true;
}
std::optional<std::uint32_t> JitEmissionQueue::block_end(std::uint64_t descriptor) const
{
    const std::lock_guard lock{impl_->mutex};
    if (impl_->job && impl_->job->descriptor == descriptor &&
        !impl_->job->token->finished())
        return impl_->job->end;
    return std::nullopt;
}
void JitEmissionQueue::wait()
{
    std::shared_ptr<Impl::Job> job;
    { const std::lock_guard lock{impl_->mutex}; job = impl_->job; }
    if (job)
        job->token->wait_finished();
}
void JitEmissionQueue::cancel()
{
    std::shared_ptr<Impl::Job> job;
    { const std::lock_guard lock{impl_->mutex}; job = std::move(impl_->job); }
    if (job) {
        job->token->cancel();
        impl_->service->wake();
        job->token->wait_finished();
    }
}
bool JitHotBlockPolicy::observe(std::uint64_t descriptor, std::uint64_t epoch, std::uint64_t generation)
{
    if (epoch != epoch_ || generation != generation_ || samples_.size() == 4096) {
        samples_.clear();
        epoch_ = epoch;
        generation_ = generation;
    }
    auto& count = samples_[descriptor];
    if (count == 8)
        return false;
    return ++count == 8;
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
        nanoseconds_.load(), fallbacks_.load(), demand_requests_.load(), optimized_.load()};
}
execution::RunResult TieredInterpreter::run(CpuThreadState& state,
    execution::InstructionSource& source, const execution::RunRequest& request)
{
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const auto entry = state;
    // Reuse the compile-cost history already maintained for host admission.
    // Pass the local deadline into the interpreter: checking only between
    // 32-instruction batches let memory synchronization overrun the quantum.
    const auto quantum = std::chrono::nanoseconds{std::clamp<std::uint64_t>(
        performance_counters().jit_block_compile_p95_nanoseconds(), 25'000, 200'000)};
    auto bounded = request;
    bounded.deadline = std::min(request.deadline, started + quantum);
    if (block_end_) {
        bounded.block_end_pc = block_end_(state);
        bounded.stop_at_control_flow = true;
        // A bounded translated block supplies the handoff boundary. Do not
        // create interior cold entries at an arbitrary interpreter quantum.
        bounded.deadline = request.deadline;
    }
    execution::RunResult result;
    bool ready = false;
    for (;;) {
        ready = ready_(state);
        // Interpret only while the originating block is being compiled. Its
        // publication ends the overlap even if execution has moved past it.
        if (ready || Clock::now() >= bounded.deadline ||
            !compiling_(entry)) {
            result.pc = state.registers[15];
            result.reason = execution::StopReason::BackendSwitch;
            break;
        }
        result = execution::ArmInterpreter::run_slice(state, source, bounded,
            context_, block_end_ ? UINT64_MAX : 32, result.ticks_consumed);
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
