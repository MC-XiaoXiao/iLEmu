// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "kernel/kernel.hpp"
#include "kernel/process_resource_usage.hpp"
#include "../../mach/task/cpu_time.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace ilemu {
ProcessResourceUsage ProcessResourceUsage::from_task(
    const XnuTaskStatistics& statistics, std::uint32_t maximum_resident_bytes)
{
    ProcessResourceUsage result;
    if (statistics.ticks_per_second != 0) {
        // calcru adds TASK_BASIC_INFO terminated time to TASK_THREAD_TIMES_INFO.
        // Reuse their conversion, including truncation of individual threads.
        std::array<std::uint32_t, 8> terminated { };
        task_mig::CpuTime::write(darwin::mach::task_info::basic_32_flavor,
            statistics, terminated);
        result.words_[0] = terminated[3];
        result.words_[1] = terminated[4];
        ProcessResourceUsage live;
        task_mig::CpuTime::write(darwin::mach::task_info::thread_times_flavor,
            statistics, live.words_);
        result.add(live);
    }
    result.words_[4] = maximum_resident_bytes;
    // HLE host duration is not guest kernel CPU time. Event counters remain
    // zero until the corresponding native task accounting source exists.
    return result;
}

void ProcessResourceUsage::add(const ProcessResourceUsage& other)
{
    for (const auto offset : { 0U, 2U }) {
        const auto microseconds = words_[offset + 1] + other.words_[offset + 1];
        words_[offset] += other.words_[offset] + microseconds / 1000000U;
        words_[offset + 1] = microseconds % 1000000U;
    }
    words_[4] = std::bit_cast<std::uint32_t>(std::max(
        std::bit_cast<std::int32_t>(words_[4]),
        std::bit_cast<std::int32_t>(other.words_[4])));
    for (std::size_t index = 5; index < words_.size(); ++index)
        words_[index] += other.words_[index];
}

bool ProcessResourceUsage::copyout(AddressSpace& memory, std::uint32_t address) const
{
    if (address > std::numeric_limits<std::uint32_t>::max() -
            darwin::resource::rusage_arm32_size + 1U)
        return false;
    for (std::size_t index = 0; index < words_.size(); ++index) {
        if (!memory.write32(address + static_cast<std::uint32_t>(index * 4U), words_[index]))
            return false;
    }
    return true;
}

ProcessResourceUsage CompatibilityKernel::collect_resource_usage() const
{
    const auto statistics = task_statistics_query_
        ? task_statistics_query_(process_.pid, true) : std::nullopt;
    const auto peak = shared_state_->darwin_abi.resource_accounting ==
            DarwinResourceAccounting::ResidentPeak
        ? static_cast<std::uint32_t>(memory_.resident_page_statistics().maximum *
              AddressSpace::page_size) : 0U;
    return ProcessResourceUsage::from_task(statistics.value_or(XnuTaskStatistics { }), peak);
}
} // namespace ilemu
