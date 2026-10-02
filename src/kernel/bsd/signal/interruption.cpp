// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace ilemu {

bool CompatibilityKernel::interrupt_thread_wait(Cpu& cpu, bool restart)
{
    const auto processor = cpu.processor_id();
    // XNU sleep returns ERESTART only for interruptible BSD operations.
    // select/poll/kevent, connect, psynch and Mach waits have their own
    // interruption results. Queued writes here have transferred no bytes.
    const bool restartable = restart &&
        (pending_waits_.contains(processor) ||
            pending_recvmsgs_.contains(processor) ||
            pending_socket_reads_.contains(processor) ||
            pending_host_accepts_.contains(processor) ||
            pending_host_writes_.contains(processor) ||
            pending_baseband_writes_.contains(processor) ||
            pending_unix_accepts_.contains(processor) ||
            pending_flocks_.contains(processor) ||
            pending_record_locks_.contains(processor));
    std::optional<BsdSyscallContext> entry;
    if (restartable) {
        if (const auto saved = pending_bsd_entries_.find(processor);
            saved != pending_bsd_entries_.end())
            entry = saved->second;
    }
    bool completed = false;
    if (const auto send = pending_mach_sends_.find(processor);
        send != pending_mach_sends_.end()) {
        {
            std::lock_guard lock { shared_state_->mach_mutex };
            using State = KernelSharedState::MachSendWaiters::State;
            const auto& ticket = send->second.ticket;
            if (!ticket->ready(shared_state_->clock.now()))
                ticket->state.store(State::Interrupted, std::memory_order_release);
        }
        completed = complete_pending_mach_send(cpu);
        // A granted send wins. Its combined receive can still encounter the
        // pending abort at a second wait within the same kernel invocation.
    }
    if (!completed && pending_mach_receives_.contains(processor)) {
        completed = deliver_pending_mach_if_ready_locked(cpu, true);
        if (!completed) {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            shared_state_->cancel_mach_receives_locked(process_.pid,
                static_cast<std::uint32_t>(processor));
            pending_mach_receives_.erase(processor);
            cpu.registers()[0] = darwin::mach_message::receive_interrupted;
            completed = true;
        }
    }
    if (!completed && has_pending_event_locked(processor))
        completed = deliver_pending_io_locked(cpu);
    if (!completed) {
        if (const auto timer = pending_timers_.find(processor);
            timer != pending_timers_.end()) {
            const auto pending = timer->second;
            pending_timers_.erase(timer);
            complete_timer(cpu, pending, pending.kind == PendingTimerKind::ThreadSwitch
                                             ? darwin::mach::success
                                             : darwin::mach::aborted);
            completed = true;
        } else if (const auto pending = pending_semaphore_waits_.find(processor);
                   pending != pending_semaphore_waits_.end()) {
            {
                std::lock_guard lock { shared_state_->mach_mutex };
                if (auto* semaphore = pending_semaphore_state_locked(pending->second))
                    std::erase(semaphore->waiters,
                        std::pair { process_.pid, static_cast<std::uint32_t>(processor) });
            }
            if (pending->second.bsd_result)
                bsd_error(cpu, darwin::error::interrupted);
            else
                cpu.registers()[0] = darwin::mach::aborted;
            pending_semaphore_waits_.erase(pending);
            completed = true;
        } else if (pending_psynch_waits_.erase(processor)) {
            shared_state_->psynch_runtime->cancel_wait(
                { process_.pid, static_cast<std::uint32_t>(processor) });
            bsd_error(cpu, darwin::error::interrupted);
            completed = true;
        } else if (pending_signal_waits_.contains(processor)) {
            completed = complete_signal_wait(cpu, true);
        } else if (pending_waits_.erase(processor) ||
                   pending_kevents_.erase(processor) ||
                   pending_recvmsgs_.erase(processor) ||
                   pending_socket_reads_.erase(processor) ||
                   pending_host_connects_.erase(processor) ||
                   pending_host_accepts_.erase(processor) ||
                   pending_host_writes_.erase(processor) ||
                   pending_baseband_writes_.erase(processor) ||
                   pending_unix_accepts_.erase(processor) ||
                   pending_flocks_.erase(processor) ||
                   pending_record_locks_.erase(processor) ||
                   pending_polls_.erase(processor) ||
                   pending_selects_.erase(processor) ||
                   pending_signal_suspends_.erase(processor)) {
            if (entry) {
                cpu.registers() = entry->registers;
                cpu.registers()[15] -= (entry->cpsr & 0x20U) ? 2U : 4U;
                cpu.set_cpsr(entry->cpsr);
            } else {
                bsd_error(cpu, darwin::error::interrupted);
            }
            completed = true;
        }
    }
    if (completed) {
        signal_state_.resume(processor);
        cpu.clear_halt();
        pending_io_poll_cache_.erase(processor);
        note_timer_deadline_transition();
        refresh_pending_event_processor_locked(processor);
        process_.waiting_for_events =
            !pending_event_processors_.empty() || !pending_waits_.empty();
    }
    return completed;
}

} // namespace ilemu
