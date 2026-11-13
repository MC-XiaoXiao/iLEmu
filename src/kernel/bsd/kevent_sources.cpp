/* SPDX-License-Identifier: MPL-2.0 */
// Evaluate retained knote sources without consuming their pending events.
// XNU reference: bsd/kern/kern_event.c, filt_proc/user/timer and knote_activate.
#include "kernel/kernel.hpp"
#include "kernel/darwin_kqueue_abi.hpp"

namespace ilemu {

std::optional<CompatibilityKernel::KeventSourceState>
CompatibilityKernel::kevent_source_state(
    const KeventRegistration& registration) const
{
    using namespace darwin::kqueue;
    switch (registration.filter) {
    case filter_process: {
        KeventSourceState state;
        std::lock_guard lock { shared_state_->mach_mutex };
        const auto process = shared_state_->process_kevent_states.find(
            static_cast<std::uint32_t>(registration.ident));
        if (process != shared_state_->process_kevent_states.end()) {
            state.process_exec_generation = process->second.exec_generation;
            state.process_exit_generation = process->second.exit_generation;
            if ((registration.filter_flags & process_note_exec) != 0U &&
                process->second.exec_generation !=
                    registration.process_exec_generation) {
                state.filter_flags |= process_note_exec;
            }
            if ((registration.filter_flags & process_note_exit) != 0U &&
                process->second.exit_generation !=
                    registration.process_exit_generation) {
                state.filter_flags |= process_note_exit;
                state.available = process->second.wait_status;
                state.result_flags |= event_end_of_file | event_one_shot;
            }
        }
        state.result_flags |= event_clear;
        state.ready = state.filter_flags != 0U;
        return state;
    }
    case filter_vnode: {
        const auto flags = registration.vnode_watch
            ? registration.vnode_watch->pending(
                  *shared_state_->guest_file_generation_registry) &
                  registration.filter_flags
            : 0U;
        return KeventSourceState {
            .ready = flags != 0U,
            .filter_flags = flags,
            .available = 0U,
        };
    }
    case filter_timer:
        return KeventSourceState {
            .ready = registration.timer &&
                registration.timer->expirations(shared_state_->clock.now()) != 0,
        };
    case filter_user:
        return KeventSourceState {
            .ready = registration.user_triggered,
            .filter_flags = registration.filter_flags,
            .available = static_cast<std::uint32_t>(registration.data),
        };
    case filter_read:
        if (const auto policy = network_policy_descriptors_.find(
                static_cast<std::uint32_t>(registration.ident));
            policy != network_policy_descriptors_.end()) {
            return KeventSourceState {
                .ready = policy->second->readable(),
                .available = static_cast<std::uint32_t>(registration.data),
                .generation = policy->second->generation(),
            };
        }
        return std::nullopt;
    case filter_write:
    case filter_mach_port:
        return std::nullopt;
    default:
        return KeventSourceState { };
    }
}

} // namespace ilemu
