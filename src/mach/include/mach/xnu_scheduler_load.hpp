/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "foundation/darwin_version.hpp"
#include "mach/xnu_qos_policy.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ilemu {

struct SchedulerLoadProfile {
    bool priority_buckets { };

    static constexpr SchedulerLoadProfile for_version(DarwinVersion version)
    {
        return { version >= DarwinGeneration::Ios10 };
    }
};

// XNU3789 priority.c/sched_average.c: runnable load follows base priority,
// independently of foreground identity or a task's requested QoS class.
class XnuSchedulerLoad {
public:
    enum class Bucket : std::size_t { Fixed, Foreground, Utility, Background };

    explicit XnuSchedulerLoad(SchedulerLoadProfile profile) : profile_ { profile } { }

    static Bucket bucket(bool timeshare, std::int32_t base_priority)
    {
        return !timeshare ? Bucket::Fixed
             : base_priority > XnuThreadQosPolicy { XnuQosTier::Utility }.priority() ? Bucket::Foreground
             : base_priority > XnuThreadQosPolicy { XnuQosTier::Background }.priority() ? Bucket::Utility : Bucket::Background;
    }

    void enter(Bucket bucket) { ++counts_[static_cast<std::size_t>(bucket)]; }
    void leave(Bucket bucket) { --counts_[static_cast<std::size_t>(bucket)]; }

    [[nodiscard]] std::optional<std::uint32_t> priority_shift(
        Bucket bucket, std::size_t processors, std::uint32_t fixed_shift) const
    {
        const auto foreground = counts_[static_cast<std::size_t>(Bucket::Foreground)];
        const auto utility = counts_[static_cast<std::size_t>(Bucket::Utility)];
        const auto background = counts_[static_cast<std::size_t>(Bucket::Background)];
        auto threads = foreground + utility + background;
        if (profile_.priority_buckets) {
            switch (bucket) {
            case Bucket::Fixed: return std::nullopt;
            case Bucket::Foreground:
                threads = foreground + std::min(utility, processors) + (background != 0);
                break;
            case Bucket::Utility: threads = utility + (background != 0); break;
            case Bucket::Background: threads = background; break;
            }
        }
        if (threads <= processors)
            return std::nullopt;
        const auto load = std::min(threads / processors, std::size_t { 127 });
        const auto shift = static_cast<std::uint32_t>(std::bit_width(load) - 1U);
        return fixed_shift > shift ? fixed_shift - shift : 0U;
    }

private:
    SchedulerLoadProfile profile_;
    std::array<std::size_t, 4> counts_ { };
};

} // namespace ilemu
