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
void CompatibilityKernel::dispatch_mach_task_trap(Cpu& cpu, std::uint32_t trap)
{
    auto& registers = cpu.registers();
    switch (trap) {
    case 26: { // mach_reply_port
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto port = shared_state_->allocate_mach_object();
        static_cast<void>(
            shared_state_->mach_port_objects.create(port, process_.pid));
        registers[0] =
            shared_state_->mach_namespaces
                .allocate(process_.pid, port,
                    xnu::ipc::type_mask(xnu::ipc::Right::Receive))
                .value_or(0);
        return;
    }
    case 28: // task_self_trap
    case 29: { // host_self_trap
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto task = mach_task_identity::control_port_locked(*shared_state_, process_);
        const auto object = mach_task_identity::special_port_locked(
            *shared_state_, task, trap == 28 ? 1U : 2U);
        if (object == xnu::ipc::null_name) registers[0] = xnu::ipc::null_name;
        else if (!shared_state_->mach_port_objects.contains(object))
            registers[0] = xnu::ipc::dead_name;
        else
            registers[0] = shared_state_->mach_namespaces.copyout(process_.pid, object,
                xnu::ipc::type_mask(xnu::ipc::Right::Send)).value_or(0U);
        return;
    }
    case 44: { // task_name_for_pid(target_task, pid, task_name_out)
        constexpr std::uint32_t kern_failure = 5;
        const auto target_task = registers[0];
        const auto requested_pid = registers[1];
        const auto output_address = registers[2];
        std::uint32_t result_port = 0; // MACH_PORT_NULL on failure
        std::uint32_t result = kern_failure;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            const auto caller_task_object =
                resolve_name_with_right(*shared_state_, process_.pid,
                    target_task, xnu::ipc::Right::Send);
            const auto caller_task =
                caller_task_object
                    ? shared_state_->task_port_pids.find(*caller_task_object)
                    : shared_state_->task_port_pids.end();
            const auto target_process =
                shared_state_->processes.find(requested_pid);
            if (caller_task != shared_state_->task_port_pids.end() &&
                caller_task->second == process_.pid &&
                target_process != shared_state_->processes.end() &&
                !target_process->second.exited &&
                (requested_pid == process_.pid || process_.effective_uid == 0 ||
                    (target_process->second.uid == process_.effective_uid &&
                        target_process->second.effective_uid ==
                            process_.effective_uid))) {
                const auto object = mach_task_identity::name_port_locked(*shared_state_, requested_pid);
                if (object != xnu::ipc::null_name) {
                    result_port =
                        shared_state_->mach_namespaces
                            .copyout(process_.pid, object,
                                xnu::ipc::type_mask(
                                    xnu::ipc::Right::Send))
                            .value_or(0);
                    if (result_port != 0)
                        result = 0; // KERN_SUCCESS
                }
            }
        }
        // XNU deliberately ignores copyout failure for these legacy traps.
        static_cast<void>(memory_.write32(output_address, result_port));
        registers[0] = result;
        return;
    }
    case 45: { // task_for_pid(target_task, pid, task_name_out)
        constexpr std::uint32_t kern_failure = 5;
        const auto target_task = registers[0];
        const auto requested_pid = registers[1];
        const auto output_address = registers[2];
        std::uint32_t result_port = 0; // MACH_PORT_NULL on failure
        std::uint32_t result = kern_failure;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            const auto caller_task_object =
                resolve_name_with_right(*shared_state_, process_.pid,
                    target_task, xnu::ipc::Right::Send);
            const auto caller_task =
                caller_task_object
                    ? shared_state_->task_port_pids.find(*caller_task_object)
                    : shared_state_->task_port_pids.end();
            const auto target_process =
                shared_state_->processes.find(requested_pid);
            if (caller_task != shared_state_->task_port_pids.end() &&
                caller_task->second == process_.pid &&
                target_process != shared_state_->processes.end() &&
                !target_process->second.exited &&
                (requested_pid == process_.pid || process_.effective_uid == 0 ||
                    target_process->second.uid == process_.effective_uid)) {
                const auto task_port =
                    std::find_if(shared_state_->task_port_pids.begin(),
                        shared_state_->task_port_pids.end(),
                        [requested_pid](const auto& entry) {
                            return entry.second == requested_pid;
                        });
                if (task_port != shared_state_->task_port_pids.end()) {
                    result_port = shared_state_->mach_namespaces
                                      .copyout(process_.pid, task_port->first,
                                          xnu::ipc::type_mask(
                                              xnu::ipc::Right::Send))
                                      .value_or(0);
                    if (result_port != 0)
                        result = 0; // KERN_SUCCESS
                }
            }
        }
        // XNU deliberately ignores copyout failure for these legacy traps.
        static_cast<void>(memory_.write32(output_address, result_port));
        registers[0] = result;
        return;
    }
    case 46: { // pid_for_task(task_name, pid_out)
        constexpr std::uint32_t kern_failure = 5;
        const auto task_name = registers[0];
        const auto output_address = registers[1];
        std::uint32_t pid = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t result = kern_failure;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            const auto task_object = resolve_name_with_right(*shared_state_,
                process_.pid, task_name, xnu::ipc::Right::Send);
            if (const auto task =
                    task_object
                        ? shared_state_->task_port_pids.find(*task_object)
                        : shared_state_->task_port_pids.end();
                task != shared_state_->task_port_pids.end() &&
                shared_state_->processes.contains(task->second)) {
                pid = task->second;
                result = 0; // KERN_SUCCESS
            }
        }
        // Darwin 8 writes -1 even when port_name_to_task fails, and ignores a
        // copyout error when selecting the trap's return code.
        static_cast<void>(memory_.write32(output_address, pid));
        registers[0] = result;
        return;
    }
    default:
        registers[0] = darwin::mach::invalid_argument;
        return;
    }
}
} // namespace ilemu
