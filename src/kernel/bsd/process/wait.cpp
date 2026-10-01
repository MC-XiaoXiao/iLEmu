// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"

#include <algorithm>
#include <mutex>

namespace ilemu {

bool CompatibilityKernel::try_wait(Cpu& cpu, const PendingWait& request)
{
    // XNU kern_exit.c holds a child busy from selection through copyout and
    // reaping. Keep that ownership here instead of splitting it between the
    // syscall and runtime poller; a copyout error must retain the zombie.
    std::lock_guard lock { shared_state_->mach_mutex };
    bool has_child = false;
    for (auto child = shared_state_->processes.begin();
        child != shared_state_->processes.end(); ++child) {
        const auto pid = child->first;
        const auto& record = child->second;
        if (record.parent_pid != process_.pid ||
            !request.matches(pid, record.membership->group()))
            continue;
        has_child = true;
        if (!record.exited)
            continue;
        const auto status = record.termination_signal != 0
                                ? record.termination_signal & 0x7fU
                                : (record.exit_status & 0xffU) << 8U;
        if (request.status_address != 0 &&
            !memory_.write32(request.status_address, status)) {
            bsd_error(cpu, darwin::error::bad_address);
            return true;
        }
        shared_state_->processes.erase(child);
        if (signal_state_.needs_last_child_check(request.processor) &&
            std::none_of(shared_state_->processes.begin(),
                shared_state_->processes.end(), [this](const auto& entry) {
                    return entry.second.parent_pid == process_.pid;
                }))
            signal_state_.reaped_last_child(request.processor);
        bsd_success(cpu, pid);
        return true;
    }
    if (!has_child) {
        bsd_error(cpu, darwin::error::no_child_process);
        return true;
    }
    if (request.options & 1U) { // WNOHANG
        bsd_success(cpu, 0);
        return true;
    }
    return false;
}

void CompatibilityKernel::dispatch_wait(Cpu& cpu)
{
    const auto& registers = cpu.registers();
    auto target = static_cast<std::int32_t>(registers[0]);
    // wait4(pid=0) snapshots the calling group at entry. A later group
    // transition must not retarget a sleeping invocation.
    if (target == 0)
        target = static_cast<std::int32_t>(0U - process_.membership->group());
    const PendingWait request { target, registers[1], registers[2],
        cpu.processor_id() };
    if (try_wait(cpu, request))
        return;
    pending_waits_.insert_or_assign(cpu.processor_id(), request);
    process_.waiting_for_events = true;
    bsd_success(cpu, 0);
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
}

bool CompatibilityKernel::complete_wait(Cpu& cpu)
{
    const auto pending = pending_waits_.find(cpu.processor_id());
    if (pending == pending_waits_.end() || !try_wait(cpu, pending->second))
        return false;
    pending_waits_.erase(pending);
    pending_bsd_entries_.erase(cpu.processor_id());
    process_.waiting_for_events = !pending_waits_.empty();
    return true;
}

} // namespace ilemu
