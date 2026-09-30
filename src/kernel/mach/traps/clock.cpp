// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/kernel_clock.hpp"
#include "kernel/mach_clock_abi.hpp"
#include "kernel/mach_scheduler_abi.hpp"
#include "../support.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

namespace ilemu {
using namespace mach_support;
void CompatibilityKernel::dispatch_mach_clock_trap(Cpu& cpu, std::uint32_t trap)
{
    auto& registers = cpu.registers();
    switch (trap) {
    case 3: // iOS ARM fast trap: mach_absolute_time
    {
        const auto absolute_time = shared_state_->clock.now();
        registers[0] = static_cast<std::uint32_t>(absolute_time);
        registers[1] = static_cast<std::uint32_t>(absolute_time >> 32U);
    }
        return;
    case 89: { // mach_timebase_info_trap
        // absolute_time_ is expressed directly in nanoseconds.
        const auto address = registers[0];
        if (!memory_.write32(address, 1) || !memory_.write32(address + 4, 1)) {
            registers[0] = 1; // KERN_INVALID_ADDRESS
        } else {
            registers[0] = 0;
        }
        return;
    }
    case 90: { // mach_wait_until_trap
        // XNU passes the absolute deadline as a little-endian 64-bit
        // argument in r0/r1. Suspend only this guest thread; the cooperative
        // scheduler wakes it when virtual monotonic time reaches the deadline.
        const auto deadline = static_cast<std::uint64_t>(registers[0]) |
                              (static_cast<std::uint64_t>(registers[1]) << 32U);
        const auto now = shared_state_->clock.now();
        if (timer_trace_count_ < 8) {
            output_.write(
                "[timer] mach_wait_until pid=" + std::to_string(process_.pid) +
                " cpu=" + std::to_string(cpu.processor_id()) +
                " now=" + std::to_string(now) +
                " deadline=" + std::to_string(deadline) + "\n");
            ++timer_trace_count_;
        }
        registers[0] = 0; // KERN_SUCCESS
        if (deadline > now) {
            std::optional<PendingTimer::BootstrapRetry> bootstrap_retry;
            {
                std::lock_guard mach_lock { shared_state_->mach_mutex };
                const auto pending =
                    shared_state_->pending_bootstrap_retries.find(process_.pid);
                if (pending != shared_state_->pending_bootstrap_retries.end()) {
                    bootstrap_retry = std::move(pending->second);
                    shared_state_->pending_bootstrap_retries.erase(pending);
                }
            }
            // A missing service is not ready merely because launchd returned an
            // empty transfer. Keep a small retry floor; the scheduler will
            // still wake this timer early when the service generation advances.
            // Turning every failed lookup into an immediate wake creates a
            // guest-side busy loop for optional services such as
            // com.apple.musicplayer.
            constexpr std::uint64_t bootstrap_retry_backoff =
                100ULL * darwin::mach::scheduler::nanoseconds_per_millisecond;
            const auto effective_deadline =
                bootstrap_retry
                    ? std::max(deadline, now + bootstrap_retry_backoff)
                    : deadline;
            pending_timers_[cpu.processor_id()] =
                PendingTimer { effective_deadline,
                    PendingTimerKind::MachWaitUntil, std::nullopt, false,
                    std::move(bootstrap_retry) };
            process_.waiting_for_events = true;
            cpu.halt(Dynarmic::HaltReason::UserDefined5);
        }
        return;
    }
    case darwin::mach::clock::sleep_trap: {
        using namespace darwin::mach::clock;
        const auto clock_name = registers[0];
        const auto sleep_type = registers[1];
        const auto seconds = registers[2];
        const auto nanoseconds = registers[3];
        const auto wakeup_time_address = registers[4];

        bool calendar_clock = false;
        if (clock_name != null_clock_name) {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            const auto object = shared_state_->mach_namespaces.resolve(
                process_.pid, clock_name);
            const auto system_object = shared_state_->mach_namespaces.resolve(
                process_.pid, process_.clock_port);
            const auto calendar_object = shared_state_->mach_namespaces.resolve(
                process_.pid, process_.calendar_clock_port);
            if (!object ||
                (object != system_object && object != calendar_object)) {
                registers[0] = darwin::mach::invalid_argument;
                return;
            }
            calendar_clock = object == calendar_object;
        }
        if (sleep_type > maximum_sleep_type ||
            nanoseconds >= nanoseconds_per_second) {
            registers[0] = darwin::mach::invalid_value;
            return;
        }

        const auto requested =
            static_cast<std::uint64_t>(seconds) * nanoseconds_per_second +
            nanoseconds;
        const auto monotonic_now = shared_state_->clock.now();
        const auto clock_now =
            calendar_clock ? shared_state_->clock.wall_time() : monotonic_now;
        auto alarm_time = requested;
        if (sleep_type == time_relative) {
            alarm_time = requested > std::numeric_limits<std::uint64_t>::max() -
                                         clock_now
                             ? std::numeric_limits<std::uint64_t>::max()
                             : clock_now + requested;
        }
        const auto remaining =
            alarm_time > clock_now ? alarm_time - clock_now : 0;
        const auto deadline =
            remaining >
                    std::numeric_limits<std::uint64_t>::max() - monotonic_now
                ? std::numeric_limits<std::uint64_t>::max()
                : monotonic_now + remaining;

        registers[0] = darwin::mach::success;
        if (alarm_time > clock_now) {
            pending_timers_[cpu.processor_id()] =
                PendingTimer { deadline, PendingTimerKind::ClockSleep,
                    wakeup_time_address, calendar_clock, std::nullopt };
            process_.waiting_for_events = true;
            cpu.halt(Dynarmic::HaltReason::UserDefined5);
            return;
        }

        const auto current_seconds =
            static_cast<std::uint32_t>(clock_now / nanoseconds_per_second);
        const auto current_nanoseconds =
            static_cast<std::uint32_t>(clock_now % nanoseconds_per_second);
        static_cast<void>(memory_.write32(
            wakeup_time_address + timespec_seconds_offset, current_seconds));
        static_cast<void>(
            memory_.write32(wakeup_time_address + timespec_nanoseconds_offset,
                current_nanoseconds));
        return;
    }
    default:
        registers[0] = darwin::mach::invalid_argument;
        return;
    }
}
} // namespace ilemu
