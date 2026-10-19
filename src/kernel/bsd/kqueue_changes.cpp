// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Apply a knote change shared by kevent syscalls and workqueue returns.
#include "kernel/kernel.hpp"
#include "kernel/kevent_wire.hpp"
#include "support.hpp"
#include <algorithm>

namespace ilemu {

std::uint32_t CompatibilityKernel::apply_kevent_change(
    std::uint32_t queue_fd, const KeventValue& value)
{
    auto queue = kqueues_.find(queue_fd);
    if (queue == kqueues_.end())
        return bsd_support::bad_file_descriptor;
    const auto ident = value.ident;
    const auto flags = value.flags;
    const auto filter_flags = value.filter_flags;
    const auto data = value.data;
    const auto user_data = value.user_data;
    const auto signed_filter = value.filter;
    // File descriptors, task IDs and Mach names remain natural-sized.
    // Timer/user identifiers and opaque cookies retain all 64 bits.
    if (signed_filter != darwin::kqueue::filter_user &&
        signed_filter != darwin::kqueue::filter_timer &&
        ident > UINT32_MAX) {
        return bsd_support::invalid_argument;
    }
    auto found = std::find_if(queue->second.begin(),
        queue->second.end(), [&](const auto& registration) {
            return registration.ident == ident &&
                   registration.filter == signed_filter &&
                   ((registration.flags &
                       darwin::kqueue::event_udata_specific) != 0U) ==
                       ((flags & darwin::kqueue::event_udata_specific) != 0U) &&
                   ((flags & darwin::kqueue::event_udata_specific) == 0U ||
                       registration.user_data == user_data);
        });
    if ((flags & darwin::kqueue::event_delete) != 0) {
        if (found != queue->second.end() &&
            (flags & darwin::kqueue::event_enable) == 0U &&
            !found->enabled &&
            (found->flags & (darwin::kqueue::event_dispatch |
                darwin::kqueue::event_udata_specific)) ==
                (darwin::kqueue::event_dispatch |
                    darwin::kqueue::event_udata_specific)) {
            found->deferred_delete = true;
            return bsd_support::operation_in_progress;
        }
        if (found != queue->second.end())
            queue->second.erase(found);
        return 0U;
    }
    if (found != queue->second.end() && found->deferred_delete &&
        (flags & darwin::kqueue::event_enable) != 0U) {
        // XNU activates the deferred knote for a final synthetic
        // EV_DELETE event without touching the original filter.
        found->enabled = true;
        found->flags &= ~darwin::kqueue::event_disable;
        return 0U;
    }
    if ((flags & darwin::kqueue::event_add) != 0) {
        KeventRegistration registration {
            ident,
            signed_filter,
            flags,
            filter_flags,
            data,
            user_data,
        };
        registration.qos = value.qos;
        registration.extension = value.extension;
        if (found != queue->second.end())
            registration.deferred_delete = found->deferred_delete;
        if (signed_filter == darwin::kqueue::filter_timer) {
            registration.timer = KeventTimer::create(data, filter_flags,
                shared_state_->clock,
                (flags & darwin::kqueue::event_one_shot) != 0U);
            if (!registration.timer) {
                return bsd_support::invalid_argument;
            }
            registration.flags |= darwin::kqueue::event_clear;
            if ((filter_flags & darwin::kqueue::timer_note_absolute) !=
                0U)
                registration.flags |= darwin::kqueue::event_one_shot;
        }
        registration.enabled = found == queue->second.end() || found->enabled;
        if ((flags & darwin::kqueue::event_disable) != 0U)
            registration.enabled = false;
        else if ((flags & darwin::kqueue::event_enable) != 0U)
            registration.enabled = true;
        if (signed_filter == darwin::kqueue::filter_vnode) {
            if (found != queue->second.end()) {
                registration.vnode_watch = found->vnode_watch;
            } else if (const auto file = file_descriptors_.find(
                           static_cast<std::uint32_t>(ident));
                file != file_descriptors_.end()) {
                registration.vnode_watch.emplace(file->second,
                    *shared_state_->guest_file_generation_registry);
            }
        }
        if (signed_filter == darwin::kqueue::filter_user) {
            registration.user_triggered =
                (flags & darwin::kqueue::event_trigger) != 0U ||
                (filter_flags & darwin::kqueue::user_note_trigger) !=
                    0U;
            registration.filter_flags =
                filter_flags & darwin::kqueue::user_note_flags_mask;
        }
        if (signed_filter == darwin::kqueue::filter_process) {
            std::uint64_t current_exec_generation = 0;
            std::uint64_t current_exit_generation = 0;
            {
                std::lock_guard lock { shared_state_->mach_mutex };
                const auto process =
                    shared_state_->process_kevent_states.find(
                        static_cast<std::uint32_t>(ident));
                if (process !=
                    shared_state_->process_kevent_states.end()) {
                    current_exec_generation =
                        process->second.exec_generation;
                    current_exit_generation =
                        process->second.exit_generation;
                }
            }
            if (found != queue->second.end()) {
                // EV_ADD modifies an existing knote. Preserve the
                // consumed position of notes that remain selected so an
                // edge arriving before the update remains pending. A
                // newly selected note starts at the current generation
                // and does not synthesize historical activity.
                const auto retains_exec_note =
                    (found->filter_flags &
                        darwin::kqueue::process_note_exec) != 0U &&
                    (filter_flags &
                        darwin::kqueue::process_note_exec) != 0U;
                const auto retains_exit_note =
                    (found->filter_flags &
                        darwin::kqueue::process_note_exit) != 0U &&
                    (filter_flags &
                        darwin::kqueue::process_note_exit) != 0U;
                registration.process_exec_generation =
                    retains_exec_note ? found->process_exec_generation
                                      : current_exec_generation;
                registration.process_exit_generation =
                    retains_exit_note ? found->process_exit_generation
                                      : current_exit_generation;
            } else {
                registration.process_exec_generation =
                    current_exec_generation;
                registration.process_exit_generation =
                    current_exit_generation;
            }
        }
        if (found == queue->second.end()) {
            queue->second.push_back(registration);
        } else {
            *found = registration;
        }
    } else if (found != queue->second.end()) {
        if (signed_filter == darwin::kqueue::filter_user) {
            if ((flags & darwin::kqueue::event_trigger) != 0U ||
                (filter_flags & darwin::kqueue::user_note_trigger) !=
                    0U) {
                found->user_triggered = true;
                // EV_CLEAR consumes a user event's current trigger,
                // not future NOTE_TRIGGER edges with identical data.
                found->clear_delivered = false;
            }
            const auto operand =
                filter_flags & darwin::kqueue::user_note_flags_mask;
            switch (filter_flags &
                    darwin::kqueue::user_note_ff_control_mask) {
            case darwin::kqueue::user_note_ff_and:
                found->filter_flags &= operand;
                break;
            case darwin::kqueue::user_note_ff_or:
                found->filter_flags |= operand;
                break;
            case darwin::kqueue::user_note_ff_copy:
                found->filter_flags = operand;
                break;
            default:
                break;
            }
            found->data = data;
            found->user_data = user_data;
        }
        if ((flags & darwin::kqueue::event_disable) != 0) {
            found->enabled = false;
            found->flags |= darwin::kqueue::event_disable;
        } else if ((flags & darwin::kqueue::event_enable) != 0) {
            found->enabled = true;
            found->flags &= ~darwin::kqueue::event_disable;
        }
    }
    return 0U;
}

} // namespace ilemu
