// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Deliver ipc_entry send-possible notifications when a port has capacity.
// Reference: xnu-2422.1.72/osfmk/ipc/ipc_right.c, ipc_port.c.

#include "kernel/kernel_shared_state.hpp"
#include "support.hpp"

namespace ilemu {
namespace mach_support {
void enqueue_send_possible_notification_locked(KernelSharedState& state,
    std::uint32_t notify, std::uint32_t name)
{
    if (!state.mach_port_objects.contains(notify) ||
        state.mach_ports_being_removed.contains(notify))
        return;
    KernelSharedState::MachMessage message;
    message.bytes.resize(36U);
    write_little_word(message.bytes, 0U, 18U);
    write_little_word(message.bytes, 4U, 36U);
    write_little_word(message.bytes, 8U, notify);
    write_little_word(message.bytes, 20U, mach_notify_send_possible);
    write_little_word(message.bytes, 28U, 1U);
    write_little_word(message.bytes, 32U, name);
    message.destination = notify;
    message.destination_send_once_object = notify;
    state.enqueue_mach_message_locked(notify, std::move(message));
}
} // namespace mach_support

void KernelSharedState::notify_send_possible_locked(std::uint32_t destination)
{
    if (mach_send_possible_armed_destinations.erase(destination) == 0U)
        return;
    const auto port = mach_port_objects.lookup(destination);
    if (!port || mach_ports_being_removed.contains(destination))
        return;
    const bool has_capacity = port->kernel_owned || mach_message_count_locked(destination) < port->queue_limit;
    for (auto it = mach_dead_name_notifications.begin();
         it != mach_dead_name_notifications.end();) {
        auto& request = it->second;
        if (request.target_object != destination || !request.send_possible ||
            !request.armed) {
            ++it;
            continue;
        }
        const auto entry = mach_namespaces.lookup(it->first.first, it->first.second);
        const bool send_once = entry && (entry->type &
            xnu::ipc::type_mask(xnu::ipc::Right::SendOnce)) != 0U;
        if (!has_capacity && !send_once) {
            mach_send_possible_armed_destinations.insert(destination);
            ++it;
            continue;
        }
        const auto notify = request.notify_object;
        const auto name = it->first.second;
        // ipc_port_spnotify claims the token but leaves the slot in use
        // until cancellation, replacement, name deletion or port teardown.
        request.notify_object = 0U;
        request.send_possible = false;
        request.armed = false;
        ++it;
        mach_support::enqueue_send_possible_notification_locked(*this, notify, name);
    }
}

} // namespace ilemu
