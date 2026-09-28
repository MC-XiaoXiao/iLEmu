// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <mach/xnu_task_statistics.hpp>
#include <kernel/mach_task_info_abi.hpp>
#include <algorithm>
#include <limits>
#include <span>

namespace ilemu::task_mig {
// XNU task_info: BASIC excludes live threads; ABSOLUTETIME includes them.
// Conversion is read-only and uses the same guest frequency as thread_info.
class CpuTime {
public:
    static bool handles(std::uint32_t flavor)
    {
        using namespace darwin::mach::task_info;
        return flavor == absolute_time_flavor || flavor == thread_times_flavor ||
               flavor == basic_32_flavor || flavor == basic_64_flavor ||
               flavor == basic_32_peak_flavor || flavor == mach_basic_flavor;
    }
    static void write(std::uint32_t flavor, const XnuTaskStatistics& stats,
        std::span<std::uint32_t> words)
    {
        if (flavor == darwin::mach::task_info::absolute_time_flavor) {
            const auto total = static_cast<__uint128_t>(stats.terminated_user_ticks) +
                               stats.live_user_ticks;
            wide(words, 0, scale(total, 1'000'000'000, stats.ticks_per_second));
            wide(words, 4, scale(stats.live_user_ticks, 1'000'000'000, stats.ticks_per_second));
        } else {
            const bool live = flavor == darwin::mach::task_info::thread_times_flavor;
            if (!live && stats.terminated_user_ticks == 0)
                return;
            const auto us = live ? stats.live_user_microseconds
                : scale(stats.terminated_user_ticks, 1'000'000, stats.ticks_per_second);
            const auto offset = live ? 0U :
                flavor == darwin::mach::task_info::mach_basic_flavor ? 6U : 3U;
            words[offset] = static_cast<std::uint32_t>(us / 1'000'000);
            words[offset + 1] = static_cast<std::uint32_t>(us % 1'000'000);
        }
    }
private:
    static std::uint64_t scale(__uint128_t ticks, std::uint64_t units, std::uint32_t frequency)
    {
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (ticks <= maximum) {
            const auto value = static_cast<std::uint64_t>(ticks);
            const auto scaled = static_cast<__uint128_t>(value / frequency) * units +
                                (value % frequency) * units / frequency;
            return static_cast<std::uint64_t>(std::min<__uint128_t>(scaled, maximum));
        }
        return static_cast<std::uint64_t>(std::min<__uint128_t>(
            ticks * units / frequency, maximum));
    }
    static void wide(std::span<std::uint32_t> words, std::size_t offset, std::uint64_t value)
    {
        words[offset] = static_cast<std::uint32_t>(value);
        words[offset + 1] = static_cast<std::uint32_t>(value >> 32U);
    }
};
}
