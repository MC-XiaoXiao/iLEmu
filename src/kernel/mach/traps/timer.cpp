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
void CompatibilityKernel::dispatch_mach_timer_trap(Cpu& cpu, std::uint32_t trap)
{
    auto& registers = cpu.registers();
    switch (trap) {
    case 91: { // mk_timer_create_trap
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto object = shared_state_->allocate_mach_object();
        shared_state_->mach_timers.emplace(object,
            KernelSharedState::MachTimer { process_.pid, std::nullopt });
        static_cast<void>(
            shared_state_->mach_port_objects.create(object, process_.pid));
        shared_state_->mach_queues.try_emplace(object);
        const auto name = shared_state_->mach_namespaces.allocate(process_.pid,
            object, xnu::ipc::type_mask(xnu::ipc::Right::Receive));
        if (!name) {
            shared_state_->mach_timers.erase(object);
            remove_port_object_locked(*shared_state_, object);
            registers[0] = xnu::ipc::null_name;
            return;
        }
        registers[0] = *name;
        output_.write("[timer] create pid=" + std::to_string(process_.pid) +
                      " name=" + std::to_string(*name) +
                      " object=" + std::to_string(object) + "\n");
        return;
    }
    case 92: { // mk_timer_destroy_trap
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto name = registers[0];
        const auto object = resolve_name_with_right(
            *shared_state_, process_.pid, name, xnu::ipc::Right::Receive);
        const auto timer = object ? shared_state_->mach_timers.find(*object)
                                  : shared_state_->mach_timers.end();
        if (timer == shared_state_->mach_timers.end()) {
            registers[0] = darwin::mach::invalid_argument;
            return;
        }
        shared_state_->mach_timer_deadlines.erase(*object);
        shared_state_->mach_timers.erase(timer);
        // Tear down the task-local receive name through the common ipc_right
        // path. It marks foreign Send names dead and drains queued rights
        // before removing the backing timer port.
        registers[0] =
            destroy_port_name_locked(*shared_state_, process_.pid, name)
                ? darwin::mach::success
                : darwin::mach::invalid_argument;
        return;
    }
    case 93: { // mk_timer_arm_trap
        const auto name = registers[0];
        const auto deadline = static_cast<std::uint64_t>(registers[1]) |
                              (static_cast<std::uint64_t>(registers[2]) << 32U);
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto object = resolve_name_with_right(
            *shared_state_, process_.pid, name, xnu::ipc::Right::Receive);
        const auto timer = object ? shared_state_->mach_timers.find(*object)
                                  : shared_state_->mach_timers.end();
        if (timer == shared_state_->mach_timers.end()) {
            registers[0] = darwin::mach::invalid_argument;
            return;
        }
        timer->second.deadline = deadline;
        shared_state_->mach_timer_deadlines.upsert(*object, deadline);
        registers[0] = darwin::mach::success;
        output_.write("[timer] arm pid=" + std::to_string(process_.pid) +
                      " name=" + std::to_string(name) +
                      " object=" + std::to_string(*object) +
                      " deadline=" + std::to_string(deadline) + "\n");
        return;
    }
    case 94: { // mk_timer_cancel_trap
        const auto name = registers[0];
        const auto result_address = registers[1];
        std::uint64_t armed_time = 0;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            const auto object = resolve_name_with_right(*shared_state_,
                process_.pid, name, xnu::ipc::Right::Receive);
            const auto timer = object ? shared_state_->mach_timers.find(*object)
                                      : shared_state_->mach_timers.end();
            if (timer == shared_state_->mach_timers.end()) {
                registers[0] = darwin::mach::invalid_argument;
                return;
            }
            armed_time = timer->second.deadline.value_or(0);
            timer->second.deadline.reset();
            shared_state_->mach_timer_deadlines.erase(*object);
        }
        if (result_address != 0 &&
            (!memory_.write32(
                 result_address, static_cast<std::uint32_t>(armed_time)) ||
                !memory_.write32(result_address + 4,
                    static_cast<std::uint32_t>(armed_time >> 32U)))) {
            registers[0] = darwin::mach::failure;
        } else {
            registers[0] = darwin::mach::success;
        }
        return;
    }
    default:
        registers[0] = darwin::mach::invalid_argument;
        return;
    }
}
} // namespace ilemu
