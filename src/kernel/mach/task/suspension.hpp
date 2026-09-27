// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "../support.hpp"
#include "kernel/darwin_abi.hpp"
#include <algorithm>
#include <limits>

namespace ilemu::task_mig {
// XNU task.c: task_suspend/task_resume (792, 1699, 2050, 2422, 2782),
// and ipc_kmsg.c: ipc_kmsg_copyout_object (2782).
// Caller holds mach_mutex. All user holds together own one scheduler hold;
// SIGSTOP and individual thread suspensions remain independent.
class Suspension {
public:
    static std::uint32_t change_locked(KernelSharedState& state,
        std::uint32_t caller, std::uint32_t pid, bool resume, bool pid_call = false)
    {
        const auto found = state.processes.find(pid);
        if (found == state.processes.end())
            return darwin::mach::invalid_argument;
        auto& task = found->second;
        if (task.exited)
            return darwin::mach::failure;
        const auto abi = state.darwin_abi.task_suspension;
        const bool protected_pid = abi != DarwinTaskSuspensionAbi::SharedUserCount;
        const bool token = !pid_call && abi == DarwinTaskSuspensionAbi::ResumePortTokens;
        if (!resume) {
            if ((pid_call && protected_pid && task.pid_suspended) ||
                task.task_user_stop_count == std::numeric_limits<std::uint32_t>::max())
                return darwin::mach::failure;
            if (token && !prepare_token_locked(state, task, pid))
                return darwin::mach::resource_shortage;
            if (pid_call)
                task.pid_suspended = true;
            if (token)
                ++task.task_legacy_stop_count;
            if (task.task_user_stop_count++ == 0 && state.task_runnable_handler)
                state.task_runnable_handler(pid, false);
            if (token && !state.mach_namespaces.copyout(caller, task.task_resume_port,
                             xnu::ipc::type_mask(xnu::ipc::Right::Send))) {
                no_senders_locked(state, task.task_resume_port);
                return darwin::mach_message::ipc_space;
            }
            return darwin::mach::success;
        }
        return resume_locked(state, caller, pid, task, pid_call, protected_pid, token);
    }

    static void no_senders_locked(KernelSharedState& state, std::uint32_t object)
    {
        const auto owner = state.task_resume_port_pids.find(object);
        if (owner == state.task_resume_port_pids.end() ||
            mach_support::port_has_send_rights_locked(state, object))
            return;
        const auto found = state.processes.find(owner->second);
        if (found == state.processes.end() || found->second.exited)
            return;
        auto& task = found->second;
        if (task.task_user_stop_count <= (task.pid_suspended ? 1U : 0U))
            return;
        task.task_user_stop_count -= std::min(task.task_user_stop_count, task.task_legacy_stop_count);
        task.task_legacy_stop_count = 0;
        if (task.task_user_stop_count == 0 && state.task_runnable_handler)
            state.task_runnable_handler(owner->second, true);
    }

private:
    static bool prepare_token_locked(KernelSharedState& state,
        KernelSharedState::ProcessRecord& task, std::uint32_t pid)
    {
        if (task.task_resume_port == 0) {
            const auto object = state.allocate_mach_object();
            if (!state.mach_port_objects.create(object))
                return false;
            task.task_resume_port = object;
            state.task_resume_port_pids.emplace(object, pid);
        }
        static_cast<void>(state.mach_port_objects.increment_make_send_count(task.task_resume_port));
        return true;
    }

    static std::uint32_t resume_locked(KernelSharedState& state,
        std::uint32_t caller, std::uint32_t pid, KernelSharedState::ProcessRecord& task,
        bool pid_call, bool protected_pid, bool token)
    {
        auto result = darwin::mach::failure;
        const bool eligible = !pid_call || !protected_pid || task.pid_suspended;
        if (eligible) {
            if (pid_call && protected_pid)
                task.pid_suspended = false;
            const auto floor = protected_pid && task.pid_suspended ? 1U : 0U;
            if (task.task_user_stop_count > floor) {
                if (token && task.task_legacy_stop_count != 0)
                    --task.task_legacy_stop_count;
                if (--task.task_user_stop_count == 0) {
                    task.pid_suspended = false;
                    if (state.task_runnable_handler)
                        state.task_runnable_handler(pid, true);
                }
                result = darwin::mach::success;
            }
        }
        if (token && task.task_resume_port != 0) {
            const auto name = state.mach_namespaces.name_for(caller, task.task_resume_port);
            if (name) {
                if (result == darwin::mach::success)
                    static_cast<void>(mach_support::modify_port_references_locked(
                        state, caller, *name, xnu::ipc::Right::Send, -1));
                else
                    static_cast<void>(mach_support::destroy_port_name_locked(state, caller, *name));
            }
        }
        return result;
    }
};
} // namespace ilemu::task_mig
