/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "execution/arm_state.hpp"
#include "execution/memory.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace ilemu::execution {

enum class StopReason : std::uint32_t {
    None = 0,
    TickBudget = 1U << 0,
    SingleStep = 1U << 1,
    HostDeadline = 1U << 2,
    GuestPreemption = 1U << 3,
    Cancelled = 1U << 4,
    Svc = 1U << 5,
    FetchFault = 1U << 6,
    UnsupportedInstruction = 1U << 7,
    InvalidTiming = 1U << 8,
    DataFault = 1U << 9,
};
constexpr StopReason operator|(StopReason a, StopReason b) noexcept
{
    return static_cast<StopReason>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr bool has_reason(StopReason value, StopReason flag) noexcept
{
    return (static_cast<std::uint32_t>(value) &
               static_cast<std::uint32_t>(flag)) != 0;
}

enum class ExecutionMode { Run, SingleStep };
struct RunRequest {
    std::uint64_t tick_budget;
    ExecutionMode mode = ExecutionMode::Run;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max();
};
struct RunResult {
    StopReason reason = StopReason::None;
    std::uint64_t ticks_consumed = 0;
    std::optional<std::uint32_t> svc;
    // Fault/unsupported/SVC report their instruction PC; normal exits report
    // the next unexecuted instruction. Unsupported does not retire a step.
    std::uint32_t pc = 0;
    std::optional<std::uint32_t> instruction;
    std::optional<MemoryFault> memory_fault;
};

// A lease excludes external code writes and mapping/protection changes.
// Owner checked accesses may change code/mappings, but must end native
// execution before doing so and reacquire the lease before any subsequent
// instruction. A generation, when supplied, also covers instruction timing and
// all shared aliases; otherwise compiled code must be revalidated at every
// lease entry.
class InstructionLease {
public:
    virtual ~InstructionLease() = default;
    virtual std::optional<std::uint64_t> generation() const noexcept
    {
        return { };
    }
};

// Bound at Run entry; a source owns permissions, backing lifetime and timing.
// No pointer into guest bytes or backend descriptor escapes this interface.
class InstructionSource {
public:
    virtual ~InstructionSource() = default;
    virtual std::unique_ptr<InstructionLease> acquire_code_lease()
    {
        return { };
    }
    virtual MemoryAccess* data_memory() noexcept { return nullptr; }
    virtual std::optional<std::uint32_t> fetch32(std::uint32_t address) = 0;
    virtual std::uint64_t ticks_for_instruction(
        std::uint32_t, std::uint32_t) const
    {
        return 1;
    }
};

// A backend binds architectural context and memory only for this call. Runtime
// slots may own implementations without embedding process or host UI state.
class Executor {
public:
    virtual ~Executor() = default;
    virtual RunResult run(
        CpuThreadState&, InstructionSource&, const RunRequest&) = 0;
    virtual void request_stop(StopReason) noexcept = 0;
};

// Only the execution owner accesses guest state. Other threads request a stop.
class StopRequests {
public:
    void request(StopReason reason) noexcept
    {
        pending_.fetch_or(
            static_cast<std::uint32_t>(reason), std::memory_order_release);
    }
    StopReason consume() noexcept
    {
        return static_cast<StopReason>(
            pending_.exchange(0, std::memory_order_acq_rel));
    }

private:
    std::atomic<std::uint32_t> pending_ { 0 };
};

}
