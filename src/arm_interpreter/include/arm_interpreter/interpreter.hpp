/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/arm_state.hpp"
#include "execution/run.hpp"

namespace ilemu::execution {

// Stateless guest context binding permits reuse by a runtime execution slot.
// Run calls are serialized by its owner; request_stop is thread-safe.
class ArmInterpreter final : public Executor {
public:
    RunResult run(CpuThreadState& state, InstructionSource& source,
        const RunRequest& request) override;
    void request_stop(StopReason reason) noexcept override
    {
        stops_.request(reason);
    }

    void clear_stop() noexcept override { static_cast<void>(stops_.consume()); }

private:
    StopRequests stops_;
};

}
