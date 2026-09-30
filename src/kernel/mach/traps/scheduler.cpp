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
void CompatibilityKernel::dispatch_mach_scheduler_trap(Cpu& cpu, std::uint32_t trap)
{
    auto& registers = cpu.registers();
    switch (trap) {
    case darwin::mach::scheduler::thread_switch_trap: {
        using namespace darwin::mach::scheduler;
        const auto thread_name = registers[0];
        const auto option = registers[1];
        const auto option_time = registers[2];
        if (option > maximum_switch_option) {
            registers[0] = darwin::mach::invalid_argument;
            return;
        }

        std::optional<XnuThreadId> handoff_thread;
        if (thread_name != xnu::ipc::null_name) {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            const auto object = resolve_name_with_right(*shared_state_,
                process_.pid, thread_name, xnu::ipc::Right::Send);
            const auto owner = object
                                   ? find_thread_owner(*shared_state_, *object)
                                   : std::nullopt;
            const auto oslock_option = option == switch_option_oslock_depress ||
                                       option == switch_option_oslock_wait;
            if (owner && (!oslock_option || owner->first == process_.pid) &&
                (owner->first != process_.pid ||
                    owner->second != static_cast<std::uint32_t>(
                                         cpu.processor_id()))) {
                handoff_thread = XnuThreadId { owner->first, owner->second };
            }
        }
        if (handoff_thread)
            scheduler_handoffs_[cpu.processor_id()] = *handoff_thread;

        registers[0] = darwin::mach::success;
        const auto wait_option = option == switch_option_wait ||
                                 option == switch_option_dispatch_contention ||
                                 option == switch_option_oslock_wait;
        if (wait_option && option_time != 0) {
            const auto scale = option == switch_option_dispatch_contention
                                   ? nanoseconds_per_microsecond
                                   : nanoseconds_per_millisecond;
            const auto deadline = shared_state_->clock.now() +
                                  static_cast<std::uint64_t>(option_time) *
                                      scale;
            pending_timers_[cpu.processor_id()] =
                PendingTimer { deadline, PendingTimerKind::ThreadSwitch,
                    std::nullopt, false, std::nullopt };
            process_.waiting_for_events = true;
            cpu.halt(Dynarmic::HaltReason::UserDefined5);
            return;
        }

        // Nonwaiting options remain runnable, but XNU ends this quantum.
        // UserDefined8 is a scheduler-only yield: it does not put the thread
        // on a wait queue and is cleared on the next round.
        scheduler_yields_[cpu.processor_id()] = SchedulerYieldRequest {
            option == switch_option_depress ||
                option == switch_option_oslock_depress,
            option_time
        };
        cpu.halt(Dynarmic::HaltReason::UserDefined8);
        return;
    }
    case darwin::mach::scheduler::swtch_pri_trap: {
        const auto should_yield = scheduler_runnable_query_ &&
                                  scheduler_runnable_query_(cpu.processor_id());
        if (!should_yield) {
            registers[0] = 0;
            return;
        }
        const auto quantum_milliseconds = static_cast<std::uint32_t>(
            xnu::scheduler::milliseconds_per_second /
            xnu::scheduler::default_preemption_rate);
        scheduler_yields_[cpu.processor_id()] =
            SchedulerYieldRequest { true, quantum_milliseconds };
        registers[0] = 1;
        cpu.halt(Dynarmic::HaltReason::UserDefined8);
        return;
    }
    case darwin::mach::scheduler::swtch_trap: {
        const auto should_yield = scheduler_runnable_query_ &&
                                  scheduler_runnable_query_(cpu.processor_id());
        if (!should_yield) {
            registers[0] = 0;
            return;
        }
        scheduler_yields_[cpu.processor_id()] =
            SchedulerYieldRequest { false, 0 };
        registers[0] = 1;
        cpu.halt(Dynarmic::HaltReason::UserDefined8);
        return;
    }
    default:
        registers[0] = darwin::mach::invalid_argument;
        return;
    }
}
} // namespace ilemu
