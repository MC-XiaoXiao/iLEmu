// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "kernel/mach_thread_info_abi.hpp"
#include "mach/xnu_scheduler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace ilemu::mach_support {

// XNU 792 through 4903: osfmk/kern/thread.c, thread_info_internal().
// Only the read-side ABI conversion belongs here; scheduling remains owned
// by XnuScheduler. Host syscall time is not guest kernel execution time.
class ThreadInformation {
public:
    explicit ThreadInformation(const XnuThreadStatistics& statistics)
        : statistics_ { statistics }
    {
    }

    static std::size_t word_count(std::uint32_t flavor)
    {
        using namespace darwin::mach::thread_info;
        switch (flavor) {
        case basic_flavor: return basic_word_count;
        case sched_timeshare_flavor: return sched_timeshare_word_count;
        case sched_fifo_flavor: return sched_fifo_word_count;
        case sched_rr_flavor: return sched_rr_word_count;
        default: return 0;
        }
    }

    std::uint32_t result(std::uint32_t flavor) const
    {
        using namespace darwin::mach::thread_info;
        if (flavor == sched_fifo_flavor ||
            (flavor == sched_timeshare_flavor && !statistics_.scheduling.timeshare) ||
            (flavor == sched_rr_flavor && statistics_.scheduling.timeshare))
            return invalid_policy;
        return 0;
    }

    void write(std::uint32_t flavor, std::span<std::uint32_t> words) const
    {
        using namespace darwin::mach::thread_info;
        const auto& info = statistics_.scheduling;
        if (flavor == basic_flavor) {
            const auto frequency = statistics_.ticks_per_second;
            const auto scaled_usage = static_cast<__uint128_t>(info.cpu_usage) *
                                     usage_scale / statistics_.scheduler_tick_ticks;
            const auto usage = static_cast<std::uint32_t>(
                std::min<__uint128_t>(scaled_usage * 3 / 5, usage_scale));
            const auto state = info.state != XnuThreadState::Waiting
                                   ? running_state
                               : statistics_.uninterruptible ? uninterruptible_state
                               : statistics_.suspend_count != 0 ? stopped_state
                                                                : waiting_state;
            const std::array<std::uint32_t, basic_word_count> basic {
                static_cast<std::uint32_t>(statistics_.user_ticks / frequency),
                static_cast<std::uint32_t>((statistics_.user_ticks % frequency) *
                                          1'000'000 / frequency),
                0, 0, usage, info.timeshare ? standard_policy : round_robin_policy,
                state, 0, statistics_.user_suspend_count, 0 };
            std::copy(basic.begin(), basic.end(), words.begin());
            return;
        }
        const auto base = static_cast<std::uint32_t>(info.base_priority);
        words[0] = xnu::scheduler::maximum_user_priority;
        words[1] = info.depressed ? 0U : base;
        words[2] = flavor == sched_timeshare_flavor
                       ? static_cast<std::uint32_t>(info.scheduled_priority)
                       : static_cast<std::uint32_t>(
                             static_cast<__uint128_t>(statistics_.quantum_ticks) *
                             1000 / statistics_.ticks_per_second);
        words[3] = info.depressed ? 1U : 0U;
        words[4] = info.depressed ? base : 0xffffffffU;
    }

private:
    const XnuThreadStatistics& statistics_;
};

} // namespace ilemu::mach_support
