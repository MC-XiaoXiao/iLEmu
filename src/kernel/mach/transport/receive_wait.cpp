// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "../support.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/kernel_mach_ipc.hpp"

namespace ilemu {

bool KernelSharedState::handoff_mach_message_locked(
    std::uint32_t destination, MachMessage& message)
{
    using State = MachReceiveWaiters::State;
    if (mach_receive_waiters.empty())
        return false;
    const auto queue = mach_queues.find(destination);
    // An older queued message must win over a newly arriving one.
    if (queue != mach_queues.end() && !queue->second.empty())
        return false;
    for (;;) {
        auto* selected = mach_receive_waiters.first(destination);
        auto position =
            selected ? selected->receive.wait_queue_sequence : UINT64_MAX;
        if (const auto links = mach_port_set_links_by_member.find(destination);
            links != mach_port_set_links_by_member.end()) {
            for (const auto& link : links->second) {
                if (link.wait_queue_sequence >= position)
                    continue;
                if (auto* receiver =
                        mach_receive_waiters.first(link.set_object)) {
                    selected = receiver;
                    position = link.wait_queue_sequence;
                }
            }
        }
        if (!selected)
            return false;
        const auto& receive = selected->receive;
        const auto object = *receive.receive_object;
        const auto right = receive.receive_is_port_set
                               ? xnu::ipc::Right::PortSet
                               : xnu::ipc::Right::Receive;
        if (!(receive.receive_is_port_set
                    ? mach_port_sets.contains(object)
                    : mach_port_objects.contains(object)) ||
            !mach_namespaces.owns_right(selected->task, object, right)) {
            mach_receive_waiters.complete(*selected, State::PortChanged);
            mach_queue_generation.note_enqueue(object);
            continue;
        }
        if (receive.deadline && clock.now() >= *receive.deadline) {
            mach_receive_waiters.complete(*selected, State::TimedOut);
            mach_queue_generation.note_enqueue(object);
            continue;
        }
        selected->message_size =
            static_cast<std::uint32_t>(message.bytes.size());
        if ((receive.options & darwin::mach_message::option_receive_large) !=
                0 &&
            receive.receive_size <
                mach_ipc::receive_buffer_size(message.bytes.size(),
                    receive.options, darwin_abi.mach_port_context)) {
            // LARGE reports size without consuming. Continue with the next
            // waiter before publishing this message to the port queue.
            mach_receive_waiters.complete(*selected, State::TooLarge);
            mach_queue_generation.note_enqueue(object);
            continue;
        }
        selected->destination = destination;
        selected->sequence_number =
            mach_port_objects.sequence_number(destination).value_or(0);
        static_cast<void>(
            mach_port_objects.increment_sequence_number(destination));
        selected->message.emplace(std::move(message));
        mach_receive_waiters.complete(*selected, State::MessageReady);
        mach_queue_generation.note_enqueue(object);
        // Direct delivery releases the slot now, even when the receiver is
        // suspended. Existing sender reservations and notifications share it.
        note_mach_message_dequeued_locked(destination);
        return true;
    }
}

void KernelSharedState::cancel_mach_receives_locked(
    std::uint32_t task, std::optional<std::uint32_t> processor)
{
    mach_receive_waiters.cancel(task, processor, [&](MachMessage& message) {
        if (message.destination_send_once_object) {
            mach_port_objects.release_send_once(
                *message.destination_send_once_object);
            message.destination_send_once_object.reset();
        }
        mach_support::discard_mach_message_rights_locked(*this, message);
    });
}
} // namespace ilemu
