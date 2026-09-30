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
void CompatibilityKernel::dispatch_mach_semaphore_trap(Cpu& cpu, std::uint32_t trap)
{
    auto& registers = cpu.registers();
    switch (trap) {
    case 33: { // semaphore_signal_trap
        std::optional<CompatibilityKernel::WokenThread> woken_thread;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            registers[0] = signal_semaphore_locked(
                registers[0], false, true, &woken_thread);
        }
        wake_thread_and_maybe_preempt(cpu, woken_thread);
        return;
    }
    case 34: { // semaphore_signal_all_trap
        std::vector<CompatibilityKernel::WokenThread> woken_threads;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            registers[0] = signal_semaphore_locked(
                registers[0], true, false, nullptr, &woken_threads);
        }
        wake_threads_and_maybe_preempt(cpu, woken_threads);
        return;
    }
    case 35: { // semaphore_signal_thread_trap
        std::optional<CompatibilityKernel::WokenThread> woken_thread;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            registers[0] = signal_semaphore_thread_locked(
                registers[0], registers[1], &woken_thread);
        }
        wake_thread_and_maybe_preempt(cpu, woken_thread);
        return;
    }
    case 36: // semaphore_wait_trap
        wait_on_semaphore(cpu, registers[0], 0, std::nullopt, false);
        return;
    case 37: // semaphore_wait_signal_trap
        wait_on_semaphore(cpu, registers[0], registers[1], std::nullopt, false);
        return;
    case 38: // semaphore_timedwait_trap
    case 39: { // semaphore_timedwait_signal_trap
        const auto nsec = trap == 38 ? registers[2] : registers[3];
        if (nsec >= 1'000'000'000U) {
            registers[0] = 4; // KERN_INVALID_ARGUMENT
            return;
        }
        const auto sec = trap == 38 ? registers[1] : registers[2];
        const auto interval =
            static_cast<std::uint64_t>(sec) * 1'000'000'000ULL + nsec;
        wait_on_semaphore(
            cpu, registers[0], trap == 39 ? registers[1] : 0, interval, false);
        return;
    }
    default:
        registers[0] = darwin::mach::invalid_argument;
        return;
    }
}
} // namespace ilemu
