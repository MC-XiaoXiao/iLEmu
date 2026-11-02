// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Select host cooperation budgets for scheduler-selected guest
// threads.

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>

#include "mach/xnu_scheduler.hpp"

namespace ilemu {

struct GuestExecutionBudgetRequest {
    XnuThreadId thread;
    std::chrono::nanoseconds jit_block_p99 { };
    std::optional<std::chrono::nanoseconds> host_control_delay;
    std::optional<std::chrono::nanoseconds> guest_realtime_deadline_delay;
    bool latency_sensitive { };
    std::chrono::steady_clock::time_point now { std::chrono::steady_clock::now() };
};

// Core host-execution policy for an XNU-selected guest thread. It chooses only
// the wall-clock boundary passed to Dynarmic; XNU remains the owner of runnable
// selection, priorities, quanta, realtime policy and guest preemption.
class GuestExecutionPolicy {
public:
    explicit GuestExecutionPolicy(std::chrono::nanoseconds response_period);

    [[nodiscard]] std::chrono::nanoseconds budget(const XnuScheduler& scheduler,
        const GuestExecutionBudgetRequest& request);
    void host_completed(XnuThreadId thread);
    [[nodiscard]] std::chrono::steady_clock::time_point deadline(XnuThreadId thread) const
    {
        return histories_.at(thread).deadline;
    }
    [[nodiscard]] std::optional<XnuThreadId> host_continuation(
        const XnuScheduler& scheduler, std::chrono::steady_clock::time_point now,
        bool interrupted = false);
    // A translated host-cooperative slice is expensive work rather than a
    // hot-code throughput sample. Keep its next slice at the low-latency
    // starting budget so display and input peers can run while demand JIT is
    // still building the working set.
    // Cache invalidation and deferred entries can split one continuation into
    // many short runs. Accumulate their host execution before renewing its
    // cooperation budget, without changing XNU time or priority accounting.
    XnuSliceCompletion observe(XnuThreadId thread, XnuSliceCompletion completion,
        bool translated_code = false, std::uint64_t host_execution_ns = 0,
        std::chrono::nanoseconds host_slice_budget = {});
    void forget(XnuThreadId thread);
    void forget_process(std::uint32_t process_id);

private:
    struct ThreadHistory {
        std::uint8_t saturation_level { };
        bool translation_active { };
        std::uint64_t continuation_host_ns { };
        std::uint64_t continuation_budget_ns { };
        std::chrono::steady_clock::time_point deadline { };
    };

    static constexpr std::uint8_t maximum_saturation_level = 2;

    std::chrono::nanoseconds response_period_;
    std::chrono::nanoseconds minimum_budget_;
    std::chrono::nanoseconds initial_budget_;
    std::chrono::nanoseconds latency_budget_;
    std::chrono::nanoseconds throughput_budget_;
    std::map<XnuThreadId, ThreadHistory> histories_;
    std::optional<XnuThreadId> host_continuation_;
};

} // namespace ilemu
