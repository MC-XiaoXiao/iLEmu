// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/darwin_signal_context.hpp"

#include <algorithm>
#include <mutex>

namespace ilemu {

bool CompatibilityKernel::try_wait(Cpu& cpu, PendingWait& request)
{
    // XNU kern_exit.c holds a child busy from selection through copyout and
    // reaping. Keep that ownership here instead of splitting it between the
    // syscall and runtime poller; a copyout error must retain the zombie.
    std::lock_guard lock { shared_state_->mach_mutex };
    auto& generation = shared_state_->processes.at(process_.pid).child_wait_generation;
    // Native wait4 sleeps on the parent process. A continued flag alone
    // does not wake that channel; unrelated scheduler polling must not do so.
    if (request.wake_generation == generation)
        return false;
    const TaskVmEvents::Scope user_access { memory_.task_vm_events() };
    request.wake_generation = generation;
    const bool information = request.information_selector.has_value();
    const bool observe = information && (request.options & 0x20U); // WNOWAIT
    const auto contract = shared_state_->darwin_abi.child_wait;
    const auto copy_information = [&](std::uint32_t pid, std::uint32_t code,
                                      std::uint32_t status) {
        darwin::signal_context::SignalInfo info { };
        info.signal = darwin::signal::child;
        info.pid = pid;
        info.code = code;
        info.status = status;
        return memory_.copy_to_user(request.status_address, std::as_bytes(std::span { &info, 1 }));
    };
    bool has_child = false;
    for (auto child = shared_state_->processes.begin();
        child != shared_state_->processes.end(); ++child) {
        const auto pid = child->first;
        auto& record = child->second;
        if (record.parent_pid != process_.pid ||
            !request.matches(pid, record.membership->group()))
            continue;
        has_child = true;
        if (!record.exited) {
            if (information) {
                const auto stop_mask = contract == DarwinChildWaitAbi::StopOption ? 8U : 0x7fU;
                const auto event = record.child_wait_status.peek(record.signal_stopped,
                    (request.options & stop_mask) != 0, (request.options & 0x10U) != 0);
                if (!event)
                    continue;
                const bool continued = *event == ChildWaitStatus::Kind::Continue;
                const bool identified = contract == DarwinChildWaitAbi::ChildIdentity;
                const auto event_pid = continued ? record.child_wait_status.continuation_pid()
                    : identified ? pid : 0U;
                const auto code = continued ? 6U : identified ? 5U : 0U;
                if (!copy_information(event_pid, code, record.child_wait_status.stop_signal()))
                    bsd_error(cpu, darwin::error::bad_address);
                else {
                    // waitid consumes only after successful copyout.
                    if (!observe)
                        record.child_wait_status.consume(*event);
                    bsd_success(cpu, 0);
                }
                return true;
            }
            const auto status = record.child_wait_status.take(record.signal_stopped,
                request.options);
            if (status) {
                if (request.status_address != 0 &&
                    !memory_.write32(request.status_address, *status))
                    bsd_error(cpu, darwin::error::bad_address);
                else
                    bsd_success(cpu, pid);
                return true;
            }
            continue;
        }
        if (information && !(request.options & 4U)) // WEXITED
            continue;
        const auto status = record.termination_signal != 0
                                ? record.termination_signal & 0x7fU
                                : (record.exit_status & 0xffU) << 8U;
        const bool identified = contract == DarwinChildWaitAbi::ChildIdentity;
        const bool copied = information
            ? copy_information(identified ? pid : 0U,
                  identified ? (record.termination_signal != 0 ? 2U : 1U) : 0U,
                  status >> 8U)
            : request.status_address == 0 || memory_.write32(request.status_address, status);
        if (!copied) {
            bsd_error(cpu, darwin::error::bad_address);
            return true;
        }
        if (observe) {
            bsd_success(cpu, 0);
            return true;
        }
        if (request.resource_usage_address != 0 &&
            !record.exit_resource_usage.copyout(memory_, request.resource_usage_address)) {
            bsd_error(cpu, darwin::error::bad_address);
            return true;
        }
        shared_state_->processes.at(process_.pid).children_resource_usage.add(
            record.exit_resource_usage);
        shared_state_->processes.erase(child);
        // Reaping the final child wakes other waiters to return ECHILD.
        const bool no_children = std::none_of(shared_state_->processes.begin(),
            shared_state_->processes.end(), [this](const auto& entry) {
                return entry.second.parent_pid == process_.pid;
            });
        if (no_children)
            ++generation;
        if (no_children && signal_state_.needs_last_child_check(request.processor))
            signal_state_.reaped_last_child(request.processor);
        bsd_success(cpu, information ? 0U : pid);
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
    // Native msleep0 entry counts; the generation guard excludes host polling.
    ++shared_state_->processes.at(process_.pid).voluntary_context_switches;
    return false;
}

void CompatibilityKernel::dispatch_wait(Cpu& cpu, bool information)
{
    const auto& registers = cpu.registers();
    auto target = static_cast<std::int32_t>(registers[information ? 1 : 0]);
    if (information) {
        const auto option_mask = shared_state_->darwin_abi.child_wait ==
                DarwinChildWaitAbi::StopOption ? 0x3fU : 0x7fU;
        if (registers[3] == 0 || (registers[3] & ~option_mask) != 0 ||
            ((registers[0] == 1U || registers[0] == 2U) && target < 0)) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
    }
    // wait4(pid=0) snapshots the calling group at entry. A later group
    // transition must not retarget a sleeping invocation.
    if (!information && target == 0)
        target = static_cast<std::int32_t>(0U - process_.membership->group());
    PendingWait request { target, registers[information ? 2 : 1],
        information ? 0U : registers[3], registers[information ? 3 : 2],
        cpu.processor_id(), std::nullopt,
        information ? std::optional { registers[0] } : std::nullopt };
    if (try_wait(cpu, request))
        return;
    pending_waits_.insert_or_assign(cpu.processor_id(), request);
    process_.waiting_for_events = true;
    bsd_success(cpu, 0);
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
}

bool CompatibilityKernel::complete_wait(Cpu& cpu)
{
    if (process_.exited)
        return false;
    const auto pending = pending_waits_.find(cpu.processor_id());
    if (pending == pending_waits_.end() || !try_wait(cpu, pending->second))
        return false;
    pending_waits_.erase(pending);
    pending_bsd_entries_.erase(cpu.processor_id());
    process_.waiting_for_events = !pending_waits_.empty();
    return true;
}

} // namespace ilemu
