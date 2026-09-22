// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Choose parallel guest execution lanes from CPU and syscall
// activity.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

#include "mach/xnu_scheduler.hpp"

namespace ilemu {

// Core execution policy for a guest hardware profile with multiple CPUs.
// Compute-heavy threads may run together, while syscall-dense threads stay on
// the main executor so a serial compatibility-kernel call does not become a
// worker barrier for every SVC.
class GuestParallelismPolicy {
public:
    GuestParallelismPolicy(
        std::uint64_t guest_ticks_per_second, std::size_t processor_count);

    [[nodiscard]] bool should_serialize(XnuThreadId thread) const;
    // Bound both sides of a throughput comparison rather than charging short
    // tasks several complete serial quanta merely to choose an execution mode.
    [[nodiscard]] std::optional<std::uint64_t> measurement_tick_limit(
        XnuThreadId thread) const;
    void observe(XnuThreadId thread, std::uint64_t ticks_consumed,
        std::uint64_t svc_calls, std::uint64_t host_execution_ns,
        bool ran_parallel, bool translated_code = false);
    void forget(XnuThreadId thread);
    void forget_process(std::uint32_t process_id);

private:
    enum class Mode : std::uint8_t {
        ProbeParallel,
        ProbeSerial,
        Parallel,
        Serial,
    };

    struct Sample {
        std::uint64_t ticks { };
        std::uint64_t host_ns { };
        std::uint8_t count { };
    };

    struct ThreadHistory {
        std::uint8_t syscall_density_score { };
        Mode mode { Mode::ProbeParallel };
        Sample parallel;
        Sample serial;
        std::uint16_t stable_slices { };
    };

    static constexpr std::uint8_t serialize_score = 2;
    static constexpr std::uint8_t maximum_score = 4;
    static constexpr std::uint64_t minimum_parallel_intervals_per_second =
        4'000;
    static constexpr std::uint64_t minimum_sample_ticks = 50'000;
    static constexpr std::uint8_t probe_slices = 3;
    static constexpr std::uint16_t reprobe_slices = 128;

    std::uint64_t minimum_parallel_ticks_per_svc_ { };
    std::size_t processor_count_ { };
    std::map<XnuThreadId, ThreadHistory> histories_;
};

} // namespace ilemu
