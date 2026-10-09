/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/run.hpp"

namespace ilemu::execution::arm64 {
// Control polls share one Run binding; they cannot touch guest context or
// release its lease. In particular, no exception may cross generated code.
class RunControl {
public:
    RunControl(StopRequests& stops, const RunRequest& request)
        : stops_(stops), deadline_(request.deadline)
    {
    }
    StopReason poll() noexcept
    {
        auto reason = stops_.consume();
        if (deadline_ != std::chrono::steady_clock::time_point::max() &&
            std::chrono::steady_clock::now() >= deadline_)
            reason = reason | StopReason::HostDeadline;
        return reason;
    }
    static std::uint32_t poll(void* control) noexcept
    {
        return static_cast<std::uint32_t>(
            static_cast<RunControl*>(control)->poll());
    }

private:
    StopRequests& stops_;
    std::chrono::steady_clock::time_point deadline_;
};
}
