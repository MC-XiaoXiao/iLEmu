// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace ilemu {

bool CompatibilityKernel::abort_thread(
    Cpu& cpu, bool safely, bool kernel_entry_pending)
{
    // Current cancellable continuations use THREAD_ABORTSAFE in XNU: Mach
    // messages, clocks, semaphores, psynch and BSD PCATCH waits. Both abort
    // APIs may interrupt these. Accepted asynchronous filesystem operations
    // have no safe cancellation point and must finish before user return.
    static_cast<void>(safely);
    const auto processor = cpu.processor_id();
    if (kernel_entry_pending || has_pending_event_locked(processor) ||
        pending_waits_.contains(processor)) {
        pending_thread_aborts_.insert(processor);
        if (!kernel_entry_pending)
            return complete_thread_abort(cpu);
    }
    // A runnable thread consumes the abort AST before its next user
    // instruction. Do not poison a later user-initiated syscall.
    return false;
}

bool CompatibilityKernel::complete_thread_abort(Cpu& cpu)
{
    const auto processor = cpu.processor_id();
    if (!pending_thread_aborts_.contains(processor))
        return false;
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
                if (auto semaphore = shared_state_->mach_semaphores.find(
                        pending->second.semaphore);
                    semaphore != shared_state_->mach_semaphores.end())
                    std::erase(semaphore->second.waiters,
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
            bsd_error(cpu, darwin::error::interrupted);
            completed = true;
        }
    }
    if (completed) {
        cpu.clear_halt();
        pending_io_poll_cache_.erase(processor);
        note_timer_deadline_transition();
        refresh_pending_event_processor_locked(processor);
        process_.waiting_for_events =
            !pending_event_processors_.empty() || !pending_waits_.empty();
    }
    if (completed || (!has_pending_event_locked(processor) &&
                         !pending_waits_.contains(processor)))
        pending_thread_aborts_.erase(processor);
    return completed;
}

} // namespace ilemu
