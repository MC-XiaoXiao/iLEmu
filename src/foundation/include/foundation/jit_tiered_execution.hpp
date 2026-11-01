/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/context.hpp"
#include "foundation/host_resource_controller.hpp"
#include <atomic>
#include <functional>

namespace ilemu {
struct JitTieredStatistics {
    std::uint64_t requests, compiled, ready, interpreted_ticks,
        interpretation_nanoseconds, synchronous_fallbacks;
};

// Session-owned admission; use the existing worker controller and each
// process's independent Dynarmic compilation lanes.
class JitTieredExecution {
public:
    explicit JitTieredExecution(HostResourceController& resources)
        : resources_(resources) { }
    std::shared_ptr<HostWorkToken> submit(HostResourceController::CancellableWork);
    void wake() { resources_.wake(); }
    void compiled(std::uint64_t count) { compiled_.fetch_add(count, std::memory_order_relaxed); }
    void fallback() { fallbacks_.fetch_add(1, std::memory_order_relaxed); }
    void interpreted(std::uint64_t ticks, std::uint64_t nanoseconds, bool ready);
    JitTieredStatistics statistics() const;
private:
    HostResourceController& resources_;
    std::atomic<std::uint64_t> requests_{}, compiled_{}, ready_{}, ticks_{}, nanoseconds_{}, fallbacks_{};
};

// Probe between checked interpreter batches, without a live instruction lease.
class TieredInterpreter final : public execution::Executor {
public:
    using Probe = std::function<bool(const CpuThreadState&)>;
    TieredInterpreter(Probe ready, std::function<void(const CpuThreadState&)> prepare,
        std::shared_ptr<JitTieredExecution> service)
        : ready_(std::move(ready)), prepare_(std::move(prepare)), service_(std::move(service)) { }
    execution::RunResult run(CpuThreadState&, execution::InstructionSource&,
        const execution::RunRequest&) override;
    void request_stop(execution::StopReason reason) noexcept override { context_.stops.request(reason); }
    void clear_stop() noexcept override { static_cast<void>(context_.stops.consume()); }
    void clear_exclusive_state() override { context_.clear_exclusive_state(); }
private:
    Probe ready_;
    std::function<void(const CpuThreadState&)> prepare_;
    std::shared_ptr<JitTieredExecution> service_;
    execution::ExecutorContext context_;
};
}
