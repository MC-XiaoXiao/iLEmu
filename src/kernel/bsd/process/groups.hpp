// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <algorithm>
#include "kernel/kernel_shared_state.hpp"

namespace ilemu::kernel_bsd {
// XNU792--3248 bsd/kern/kern_prot.c: getpgid and setsid. Callers hold
// mach_mutex so membership publication and remote queries see one state.
class ProcessGroups {
public:
    struct Result { std::uint32_t value { }; std::uint32_t error { }; };

    static Result query(const KernelSharedState& state, std::uint32_t caller,
        std::uint32_t target, bool session = false)
    {
        const auto process = state.processes.find(target == 0U ? caller : target);
        // proc_find excludes zombies. No credential or session restriction
        // applies to getpgid, including a query made after dropping uid.
        if (process == state.processes.end() || process->second.exited)
            return { 0U, 3U }; // ESRCH
        const auto& membership = *process->second.membership;
        return { session ? membership.session : membership.group() };
    }

    static Result set_group(KernelSharedState& state, std::uint32_t caller,
        std::uint32_t pid, std::int32_t group)
    {
        const auto target = pid == 0U ? caller : pid;
        const auto found = state.processes.find(target);
        if (found == state.processes.end() || found->second.exited)
            return { 0U, 3U };
        auto& record = found->second;
        const auto session = state.processes.at(caller).membership->session;
        if (target != caller) {
            // XNU inferior() accepts descendants, not just direct children.
            auto ancestor = target;
            for (auto remaining = state.processes.size(); ancestor != caller;) {
                const auto parent = state.processes.find(ancestor);
                if (remaining-- == 0U || parent == state.processes.end() ||
                    parent->second.exited || ancestor == 0U)
                    return { 0U, 3U };
                ancestor = parent->second.parent_pid;
            }
            if (record.membership->session != session)
                return { 0U, 1U };
            if (record.has_executed)
                return { 0U, 13U }; // EACCES, before validating pgid
        }
        if (record.membership->session == target)
            return { 0U, 1U };
        if (group < 0)
            return { 0U, 22U };
        const auto destination = group == 0 ? target :
            static_cast<std::uint32_t>(group);
        if (destination != target) {
            const auto member = std::find_if(state.processes.begin(),
                state.processes.end(), [destination](const auto& entry) {
                    return entry.second.membership->group() == destination;
                });
            if (member == state.processes.end() ||
                member->second.membership->session != session)
                return { 0U, 1U };
        }
        record.membership->join(destination);
        return { };
    }

    static Result create_session(KernelSharedState& state, ProcessContext& caller)
    {
        auto& record = state.processes.at(caller.pid);
        if (record.membership->group() == caller.pid || record.in_vfork)
            return { 0U, 1U }; // EPERM
        for (const auto& [pid, member] : state.processes) {
            static_cast<void>(pid);
            // An existing group survives its leader; native leavepgrp runs
            // when the zombie is reaped, not when the process first exits.
            if (member.membership->group() == caller.pid)
                return { 0U, 1U };
        }
        caller.membership->join(caller.pid);
        caller.membership->session = caller.pid;
        return { caller.pid };
    }
};
} // namespace ilemu::kernel_bsd
