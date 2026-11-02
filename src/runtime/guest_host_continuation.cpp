/* SPDX-License-Identifier: MPL-2.0 */
#include "guest_host_continuation.hpp"

#include "graphics/gles_program_renderer.hpp"
#include "kernel/kernel.hpp"

namespace ilemu {

bool GuestHostContinuation::eligible() const
{
    const auto info = scheduler_.info(thread_);
    return info && info->state == XnuThreadState::Running &&
           info->last_processor &&
           scheduler_.preemption_for(thread_, *info->last_processor) ==
               XnuPreemption::None;
}

bool GuestHostContinuation::resume(Cpu& cpu, CpuRunResult& result,
    std::chrono::steady_clock::time_point deadline)
{
    while (kernel_.thread_awaits_host(thread_.thread) && eligible() &&
           std::chrono::steady_clock::now() < deadline) {
        // Sample before checking the retained ticket, covering publication
        // between readiness inspection and entry into the completion wait.
        const auto observed = gles_completion_generation();
        const auto started = std::chrono::steady_clock::now();
        const auto completed = kernel_.deliver_pending_event(cpu);
        result.host_execution_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
        if (kernel_.process().exited) {
            result.reason = Dynarmic::HaltReason::UserDefined1;
            return false;
        }
        if (completed) {
            result.reason = cpu.consume_requested_halt_reason();
            if (result.reason != Dynarmic::HaltReason {})
                return false;
            if (eligible() && std::chrono::steady_clock::now() < deadline)
                return true;
            result.reason = Dynarmic::HaltReason::UserDefined2;
            result.host_yielded = std::chrono::steady_clock::now() >= deadline;
            return false;
        }
        if (!kernel_.thread_awaits_host(thread_.thread) || !eligible())
            break;
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining > std::chrono::nanoseconds::zero())
            wait_gles_host_activity(observed, remaining);
    }
    return false;
}
} // namespace ilemu
