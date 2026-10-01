// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "kernel/kernel_shared_state.hpp"

namespace ilemu::kernel_bsd {
// XNU792--3248 bsd/kern/kern_prot.c: getpgid and setsid. Callers hold
// mach_mutex so membership publication and remote queries see one state.
class ProcessGroups {
public:
    struct Result { std::uint32_t value { }; std::uint32_t error { }; };

    static Result query(const KernelSharedState& state, std::uint32_t caller,
        std::uint32_t target)
    {
        const auto process = state.processes.find(target == 0U ? caller : target);
        // proc_find excludes zombies. No credential or session restriction
        // applies to getpgid, including a query made after dropping uid.
        if (process == state.processes.end() || process->second.exited)
            return { 0U, 3U }; // ESRCH
        return { process->second.process_group };
    }

    static Result create_session(KernelSharedState& state, ProcessContext& caller)
    {
        auto& record = state.processes.at(caller.pid);
        if (record.process_group == caller.pid || record.in_vfork)
            return { 0U, 1U }; // EPERM
        for (const auto& [pid, member] : state.processes) {
            static_cast<void>(pid);
            // An existing group survives its leader; native leavepgrp runs
            // when the zombie is reaped, not when the process first exits.
            if (member.process_group == caller.pid)
                return { 0U, 1U };
        }
        caller.process_group = caller.pid;
        caller.session_id = caller.pid;
        record.process_group = caller.pid;
        return { caller.pid };
    }
};
} // namespace ilemu::kernel_bsd
