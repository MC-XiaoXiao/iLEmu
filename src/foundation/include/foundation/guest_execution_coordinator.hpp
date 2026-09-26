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
#include <memory>
#include <span>

#include "foundation/cpu.hpp"

namespace ilemu {

struct GuestExecutionRequest {
    Cpu* cpu { };
    std::size_t execution_slot { };
    std::uint64_t tick_budget { };
    std::chrono::nanoseconds host_slice_budget { };
    bool single_step { };
    CpuRunResult result;
    std::exception_ptr error;
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
    // thread submits/collects; Guest kernel state is never touched by workers.
    void submit(GuestExecutionRequest& request);
    [[nodiscard]] GuestExecutionRequest* wait();
    // Execute one lane on the caller while dispatching its peers, then drain
    // the kernel commit window. Individual channels remain independently usable.
    void run(std::span<GuestExecutionRequest*> requests);
    static void execute(GuestExecutionRequest& request) noexcept;
    [[nodiscard]] static bool in_execution_channel() noexcept;

private:
    void worker_loop(std::size_t index);
    void stop() noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ilemu
