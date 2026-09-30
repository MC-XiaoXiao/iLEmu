// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/kernel.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <utility>

namespace ilemu {

void CompatibilityKernel::retire_thread_continuations(std::size_t processor)
{
    // The caller owns kernel dispatch serialization. Retire shared wait
    // registrations before dropping the continuation that owns their state.
    const auto slot = static_cast<std::uint32_t>(processor);
    shared_state_->psynch_runtime->cancel_wait(
        DarwinPsynchThread { process_.pid, slot });
    {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        shared_state_->cancel_mach_sends_locked(process_.pid, slot);
        const auto waiter = std::pair { process_.pid, slot };
        if (const auto pending = pending_semaphore_waits_.find(processor);
            pending != pending_semaphore_waits_.end()) {
            if (const auto semaphore = shared_state_->mach_semaphores.find(
                    pending->second.semaphore);
                semaphore != shared_state_->mach_semaphores.end()) {
                std::erase(semaphore->second.waiters, waiter);
            }
        }
        shared_state_->semaphore_wakeups.erase(waiter);
        shared_state_->semaphore_terminations.erase(waiter);
    }

    clear_thread_pthread_state(processor);
    disabled_thread_signals_.erase(processor);
    alternate_signal_stacks_.erase(processor);
    signal_masks_.retire(processor);
    last_delivered_graphics_inputs_.erase(processor);
    scheduler_yields_.erase(processor);
    scheduler_handoffs_.erase(processor);
    pending_waits_.erase(processor);
    pending_thread_aborts_.erase(processor);
    pending_mach_sends_.erase(processor);
    pending_mach_receives_.erase(processor);
    pending_kevents_.erase(processor);
    pending_recvmsgs_.erase(processor);
    pending_socket_reads_.erase(processor);
    pending_host_connects_.erase(processor);
    pending_host_accepts_.erase(processor);
    pending_host_writes_.erase(processor);
    // Keep accepted host effects and process-wide descriptors alive.
    // Only retire the syscall continuation, not file_rename_effects_.
    pending_file_syncs_.erase(processor);
    pending_file_renames_.erase(processor);
    pending_filesystem_dispatches_.erase(processor);
    pending_file_mappings_.erase(processor);
    pending_baseband_writes_.erase(processor);
    pending_unix_accepts_.erase(processor);
    pending_flocks_.erase(processor);
    pending_record_locks_.erase(processor);
    pending_polls_.erase(processor);
    pending_selects_.erase(processor);
    pending_timers_.erase(processor);
    pending_semaphore_waits_.erase(processor);
    pending_psynch_waits_.erase(processor);
    pending_signal_suspends_.erase(processor);
    pending_io_poll_cache_.erase(processor);
    refresh_pending_event_processor_locked(processor);
    note_timer_deadline_transition();
    process_.waiting_for_events =
        !pending_event_processors_.empty() || !pending_waits_.empty();
}

} // namespace ilemu
