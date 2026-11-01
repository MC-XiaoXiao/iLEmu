/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/context.hpp"
#include "foundation/host_resource_controller.hpp"
#include <atomic>
#include <functional>
#include <optional>
#include <unordered_map>

namespace Dynarmic::IR { class Block; }

namespace ilemu {
struct JitTieredStatistics {
    std::uint64_t requests, compiled, ready, interpreted_ticks,
        interpretation_nanoseconds, synchronous_fallbacks, demand_requests,
        optimized_blocks;
};

// Session-owned admission; use the existing worker controller and each
// process's independent Dynarmic compilation lanes.
class JitTieredExecution {
public:
    explicit JitTieredExecution(HostResourceController& resources)
        : resources_(resources) { }
    std::shared_ptr<HostWorkToken> submit(HostResourceController::CancellableWork);
    std::shared_ptr<HostWorkToken> submit_demand(HostResourceController::CancellableWork);
    void wake() { resources_.wake(); }
    void compiled(std::uint64_t count) { compiled_.fetch_add(count, std::memory_order_relaxed); }
    void optimized(std::uint64_t count) { compiled(count); optimized_.fetch_add(count, std::memory_order_relaxed); }
    void fallback() { fallbacks_.fetch_add(1, std::memory_order_relaxed); }
    void interpreted(std::uint64_t ticks, std::uint64_t nanoseconds, bool ready);
    JitTieredStatistics statistics() const;
private:
    HostResourceController& resources_;
    std::atomic<std::uint64_t> requests_{}, compiled_{}, ready_{}, ticks_{}, nanoseconds_{}, fallbacks_{};
    std::atomic<std::uint64_t> demand_requests_{}, optimized_{};
};

// One bounded, already translated demand block per address space. The Guest
// executor owns registers; the compiler only owns IR and its publication ticket.
class JitEmissionQueue {
public:
    using Emit = std::function<bool(Dynarmic::IR::Block, std::uint64_t, std::uint64_t)>;
    JitEmissionQueue(std::shared_ptr<JitTieredExecution>, Emit);
    ~JitEmissionQueue();
    bool defer(Dynarmic::IR::Block&, std::uint64_t generation, std::uint64_t revision);
    std::optional<std::uint32_t> block_end(std::uint64_t descriptor) const;
    void wait();
    void cancel();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Samples at existing execution boundaries, with no instrumentation on each
// native block. Epoch changes invalidate the bounded observation window.
class JitHotBlockPolicy {
public:
    bool observe(std::uint64_t descriptor, std::uint64_t epoch, std::uint64_t generation);
private:
    std::uint64_t epoch_{};
    std::uint64_t generation_{};
    std::unordered_map<std::uint64_t, unsigned> samples_;
};

// Probe between checked interpreter batches, without a live instruction lease.
class TieredInterpreter final : public execution::Executor {
public:
    using Probe = std::function<bool(const CpuThreadState&)>;
    using BlockEnd = std::function<std::optional<std::uint32_t>(const CpuThreadState&)>;
    TieredInterpreter(Probe ready, Probe compiling,
        std::shared_ptr<JitTieredExecution> service, BlockEnd block_end = {})
        : ready_(std::move(ready)), compiling_(std::move(compiling)), service_(std::move(service)), block_end_(std::move(block_end)) { }
    execution::RunResult run(CpuThreadState&, execution::InstructionSource&,
        const execution::RunRequest&) override;
    void request_stop(execution::StopReason reason) noexcept override { context_.stops.request(reason); }
    void clear_stop() noexcept override { static_cast<void>(context_.stops.consume()); }
    void clear_exclusive_state() override { context_.clear_exclusive_state(); }
private:
    Probe ready_;
    Probe compiling_;
    std::shared_ptr<JitTieredExecution> service_;
    BlockEnd block_end_;
    execution::ExecutorContext context_;
};
}
