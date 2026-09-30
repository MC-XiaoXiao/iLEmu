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
    const auto processor = cpu.processor_id();
    if (exception_delivery_.waiting(processor)) {
        // Exception RPC is not an abort-safe user syscall. A full abort
        // destroys its private receive right and resumes the saved context.
        if (safely) return false;
        exception_delivery_.cancel(*shared_state_, processor);
        cpu.clear_halt();
        refresh_pending_event_processor_locked(processor);
        process_.waiting_for_events = !pending_event_processors_.empty() || !pending_waits_.empty();
        return true;
    }
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
    const auto completed = interrupt_thread_wait(cpu, false);
    if (completed || (!has_pending_event_locked(processor) &&
                         !pending_waits_.contains(processor)))
        pending_thread_aborts_.erase(processor);
    return completed;
}

} // namespace ilemu
