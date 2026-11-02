/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "foundation/guest_execution_coordinator.hpp"
#include "mach/xnu_scheduler.hpp"

namespace ilemu {
class CompatibilityKernel;

// Finish pure implementation waits within the existing serial execution turn.
// The coordinator retains its absolute deadline and remaining Guest ticks.
class GuestHostContinuation final : public GuestExecutionContinuation {
public:
    GuestHostContinuation(CompatibilityKernel& kernel, XnuScheduler& scheduler,
        XnuThreadId thread)
        : kernel_ { kernel }, scheduler_ { scheduler }, thread_ { thread } { }

    bool resume(Cpu& cpu, CpuRunResult& result,
        std::chrono::steady_clock::time_point deadline) override;

private:
    [[nodiscard]] bool eligible() const;
    CompatibilityKernel& kernel_;
    XnuScheduler& scheduler_;
    XnuThreadId thread_;
};
} // namespace ilemu
