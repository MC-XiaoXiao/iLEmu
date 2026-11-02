// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Run scheduler-selected guest CPU batches on coordinated host
// execution lanes.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <span>

#include "foundation/cpu.hpp"
#include "execution/cooperation.hpp"

namespace ilemu {

struct GuestExecutionRequest {
    Cpu* cpu { };
    std::size_t execution_slot { };
    std::uint64_t tick_budget { };
    std::chrono::nanoseconds host_slice_budget { };
    bool single_step { };
    AddressSpace* exclusive_memory { };
    CpuRunResult result;
    std::exception_ptr error;
    const execution::GuestCooperationSignal* cooperation_signal { };
    std::uint64_t completion_generation { };
};

// Owns host execution lanes for a batch selected by the Guest scheduler. The
// caller supplies policy results and remains responsible for applying Guest
// completion semantics; this class only executes independent CPU requests.
class GuestExecutionCoordinator {
public:
    explicit GuestExecutionCoordinator(std::size_t worker_count);
    ~GuestExecutionCoordinator();

    GuestExecutionCoordinator(const GuestExecutionCoordinator&) = delete;
    GuestExecutionCoordinator& operator=(
        const GuestExecutionCoordinator&) = delete;

    // The caller owns a request until wait() returns it. Only the control
    // thread submits/collects. A serial window may own immediate Guest kernel
    // dispatch; concurrent windows defer dispatch until every lane completes.
    void submit(GuestExecutionRequest& request);
    // Sample before serial readiness polling; do not rebase at submission or
    // worker entry, since a completion in that gap must still interrupt CPU.
    void poll_host_completions();
    void arm_host_completions(bool pending);
    void notify_host_completion() noexcept;
    [[nodiscard]] GuestExecutionRequest* wait();
    // Guest work always stays on its channel. While the commit window is
    // open, service_host may collect host events but must not mutate Guest
    // kernel state or the requests. Completion wakes the coordinator directly.
    void run(std::span<GuestExecutionRequest*> requests,
        const std::function<void()>& service_host = {});
    static void execute(GuestExecutionRequest& request) noexcept;
    [[nodiscard]] static bool in_execution_channel() noexcept;

private:
    void worker_loop(std::size_t index);
    void stop() noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    execution::GuestCooperationSignal host_completions_;
    std::uint64_t polled_completion_generation_ { };
    bool host_completion_pending_ { };
};

} // namespace ilemu
