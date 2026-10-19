// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "calendar_notifications.hpp"

#include "kernel/darwin_abi.hpp"
#include "mach/mig_wire_abi.hpp"

namespace ilemu::host_mig {

std::uint32_t CalendarNotifications::register_locked(KernelSharedState& state,
    std::uint32_t process_id, std::uint32_t name, std::uint32_t type)
{
    if (type > calendar_set)
        return darwin::mach::invalid_argument;
    const auto object = mach_support::resolve_receive_object(
        state, process_id, name);
    if (!object || !state.mach_port_objects.contains(*object))
        return darwin::mach::invalid_capability;
    if (state.host_calendar_notifications.contains(*object))
        return darwin::mach::failure;
    state.mach_port_objects.make_send_once(*object);
    state.host_calendar_notifications.emplace(*object, type);
    return darwin::mach::success;
}

void CalendarNotifications::clock_set_locked(KernelSharedState& state)
{
    for (const auto& [object, type] : state.host_calendar_notifications) {
        if (!state.mach_port_objects.contains(object))
            continue;
        KernelSharedState::MachMessage message;
        message.bytes.resize(darwin::mig_wire::message_header_size);
        mach_support::write_little_word(message.bytes, 0U,
            darwin::mig_wire::message_bits(
                darwin::mig_wire::disposition_move_send_once));
        mach_support::write_little_word(message.bytes, 4U,
            darwin::mig_wire::message_header_size);
        mach_support::write_little_word(message.bytes, 8U, object);
        mach_support::write_little_word(message.bytes, 20U,
            type == calendar_change ? 950U : 951U);
        message.destination = object;
        message.destination_send_once_object = object;
        state.enqueue_mach_message_locked(object, std::move(message));
    }
    state.host_calendar_notifications.clear();
}

void CalendarNotifications::port_destroyed_locked(
    KernelSharedState& state, std::uint32_t object)
{
    if (state.host_calendar_notifications.erase(object) != 0U)
        state.mach_port_objects.release_send_once(object);
}

} // namespace ilemu::host_mig
