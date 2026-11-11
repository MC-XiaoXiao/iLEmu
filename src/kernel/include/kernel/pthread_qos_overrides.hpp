/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>

namespace ilemu {

// The original anonymous start/end contract retains its peak until balanced.
class DarwinPthreadQosOverride {
public:
    static constexpr std::uint32_t maximum_depth = 256U;
    [[nodiscard]] bool start(std::optional<std::int32_t> priority) noexcept;
    [[nodiscard]] bool end(bool retain_peak = false) noexcept;
    void observe_priority(std::optional<std::int32_t> priority) noexcept;
    void clear_peak_if_balanced() noexcept;
    [[nodiscard]] bool empty() const noexcept { return count_ == 0U && !priority_; }
    [[nodiscard]] std::optional<std::int32_t> priority() const noexcept
    {
        return priority_;
    }

private:
    std::uint32_t count_ { };
    std::optional<std::int32_t> priority_;
};

// Split-libpthread resource overrides retain a peak per resource. Dispatch
// donations have a separate reset scope and cannot retire an explicit lock
// donation. See libpthread-137.1.1/218.1.3 kern_support.c and XNU task_policy.c.
class DarwinPthreadResourceQosOverrides {
public:
    void start(std::uint32_t resource, std::optional<std::int32_t> priority);
    void end(std::uint32_t resource);
    void set_mutex(std::uint32_t resource, std::optional<std::int32_t> priority);
    void boost_dispatch(std::optional<std::int32_t> priority) noexcept;
    void boost_workqueue(std::optional<std::int32_t> priority) noexcept
    {
        workqueue_priority_ = std::max(workqueue_priority_, priority);
    }
    void reset_workqueue() noexcept { workqueue_priority_.reset(); }
    [[nodiscard]] bool workqueue_active() const noexcept
    {
        return workqueue_priority_.has_value();
    }
    void reset_dispatch() noexcept
    {
        dispatch_count_ = 0U;
        dispatch_priority_.reset();
    }
    [[nodiscard]] bool dispatch_active() const noexcept { return dispatch_count_ != 0U; }
    [[nodiscard]] bool empty() const noexcept
    {
        return resources_.empty() && mutexes_.empty() && !dispatch_active() && !workqueue_active();
    }
    [[nodiscard]] std::optional<std::int32_t> priority() const noexcept;

private:
    struct Donation {
        std::uint64_t count { };
        std::optional<std::int32_t> priority;
    };
    std::map<std::uint32_t, Donation> resources_;
    std::map<std::uint32_t, std::int32_t> mutexes_;
    std::optional<std::int32_t> dispatch_priority_;
    std::uint64_t dispatch_count_ { };
    std::optional<std::int32_t> workqueue_priority_;
};

} // namespace ilemu
