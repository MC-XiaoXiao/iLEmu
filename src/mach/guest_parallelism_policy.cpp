// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Choose parallel guest execution lanes from CPU and syscall
// activity.

#include "mach/guest_parallelism_policy.hpp"

#include <algorithm>

namespace ilemu {

GuestParallelismPolicy::GuestParallelismPolicy(
    std::uint64_t guest_ticks_per_second, std::size_t processor_count)
    : minimum_parallel_ticks_per_svc_ { std::max<std::uint64_t>(
          1, guest_ticks_per_second / minimum_parallel_intervals_per_second) }
    , processor_count_ { std::max<std::size_t>(1, processor_count) }
{
}

bool GuestParallelismPolicy::should_serialize(XnuThreadId thread) const
{
    const auto history = histories_.find(thread);
    return history != histories_.end() &&
           (history->second.syscall_density_score >= serialize_score ||
               history->second.mode == Mode::ProbeSerial ||
               history->second.mode == Mode::Serial);
}

std::optional<std::uint64_t> GuestParallelismPolicy::measurement_tick_limit(
    XnuThreadId thread) const
{
    const auto history = histories_.find(thread);
    // A thread can enter a compute phase immediately after initialization
    // syscalls. Bound its recovery as well as both throughput probe modes.
    if (history == histories_.end() ||
        history->second.syscall_density_score >= serialize_score ||
        history->second.mode == Mode::ProbeParallel ||
        history->second.mode == Mode::ProbeSerial)
        return minimum_sample_ticks * 4U;
    return std::nullopt;
}

void GuestParallelismPolicy::observe(XnuThreadId thread,
    std::uint64_t ticks_consumed, std::uint64_t svc_calls,
    std::uint64_t host_execution_ns, bool ran_parallel, bool translated_code)
{
    auto& history = histories_[thread];
    const auto dense = svc_calls != 0 && ticks_consumed / svc_calls <
                                             minimum_parallel_ticks_per_svc_;
    if (dense) {
        history.syscall_density_score = std::min<std::uint8_t>(
            maximum_score, history.syscall_density_score + 1U);
    } else if (history.syscall_density_score != 0U) {
        --history.syscall_density_score;
    }

    if (translated_code || svc_calls != 0U ||
        ticks_consumed < minimum_sample_ticks || host_execution_ns == 0U)
        return;

    const bool expected_parallel =
        history.mode == Mode::Parallel || history.mode == Mode::ProbeParallel;
    if (ran_parallel != expected_parallel)
        return;

    auto& sample = ran_parallel ? history.parallel : history.serial;
    if (sample.count == probe_slices)
        sample = { };
    sample.ticks += ticks_consumed;
    sample.host_ns += host_execution_ns;
    ++sample.count;

    const auto choose_mode = [&] {
        // Two concurrent guest lanes are useful only if their combined rate
        // beats two serialized fast-memory slices. Keep a margin for noise.
        const auto serial_rate =
            static_cast<long double>(history.serial.ticks) /
            history.serial.host_ns;
        const auto parallel_rate =
            static_cast<long double>(history.parallel.ticks) /
            history.parallel.host_ns;
        history.mode = serial_rate > parallel_rate * processor_count_ * 1.15L
                           ? Mode::Serial
                           : Mode::Parallel;
        history.stable_slices = 0;
    };

    if (history.mode == Mode::ProbeParallel &&
        history.parallel.count == probe_slices) {
        if (history.serial.count == 0U) {
            history.mode = Mode::ProbeSerial;
        } else {
            choose_mode();
        }
    } else if (history.mode == Mode::ProbeSerial &&
               history.serial.count == probe_slices) {
        choose_mode();
    } else if (history.mode == Mode::Parallel || history.mode == Mode::Serial) {
        if (++history.stable_slices >= reprobe_slices) {
            history.stable_slices = 0;
            if (history.mode == Mode::Parallel) {
                history.serial = { };
                history.mode = Mode::ProbeSerial;
            } else {
                history.parallel = { };
                history.mode = Mode::ProbeParallel;
            }
        }
    }
}

void GuestParallelismPolicy::forget(XnuThreadId thread)
{
    histories_.erase(thread);
}

void GuestParallelismPolicy::forget_process(std::uint32_t process_id)
{
    std::erase_if(histories_, [process_id](const auto& entry) {
        return entry.first.process == process_id;
    });
}

} // namespace ilemu
