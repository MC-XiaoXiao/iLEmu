// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Native references: XNU2422/2782 osfmk/ipc/mach_port.c and mach_kernelrpc.c.
#include "guarded.hpp"

#include "../support.hpp"
#include "foundation/address_space.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "mach/mig_wire_abi.hpp"

#include <bit>

namespace ilemu {

using namespace mach_support;
using namespace xnu::ipc;

GuardedPorts::Result GuardedPorts::construct_locked(KernelSharedState& state,
    std::uint32_t task, Options options, std::uint64_t context)
{
    constexpr std::uint32_t context_as_guard = 0x01U;
    constexpr std::uint32_t queue_limit_option = 0x02U;
    constexpr std::uint32_t temporary_owner = 0x04U;
    constexpr std::uint32_t insert_send = 0x10U;
    constexpr std::uint32_t strict = 0x20U;
    const auto flags = options.flags;
    const auto limit = options.queue_limit;
    if ((flags & queue_limit_option) != 0 && limit > maximum_queue_limit)
        return { darwin::mach::invalid_value };

    const auto object = state.allocate_mach_object();
    const auto name =
        state.mach_namespaces.allocate(task, object, type_mask(Right::Receive));
    if (!name)
        return { darwin::mach::no_space };
    static_cast<void>(state.mach_port_objects.create(object, task));
    state.mach_queues.try_emplace(object);
    state.mach_port_contexts[object] = context;
    if ((flags & context_as_guard) != 0)
        static_cast<void>(state.mach_port_objects.set_guard(
            object, context, (flags & strict) != 0));
    if ((flags & queue_limit_option) != 0)
        static_cast<void>(
            state.mach_port_objects.set_queue_limit(object, limit));
    if ((flags & temporary_owner) != 0)
        static_cast<void>(state.mach_port_objects.set_temporary_owner(object));
    // Importance/de-nap receivers use the kernel's existing queued-message
    // importance accounting; no additional host scheduling is required.
    auto result = darwin::mach::success;
    if ((flags & insert_send) != 0)
        result = insert_port_right_locked(state, task, task, *name, *name,
            darwin::mig_wire::disposition_make_send);
    if (result != darwin::mach::success)
        static_cast<void>(destroy_port_name_locked(state, task, *name));
    return { result, result == darwin::mach::success ? *name : 0U };
}

std::uint32_t GuardedPorts::change_locked(KernelSharedState& state,
    std::uint32_t task, std::uint32_t name, Operation operation,
    std::uint64_t guard, std::int32_t delta, bool strict)
{
    const auto entry = state.mach_namespaces.lookup(task, name);
    if (!entry)
        return darwin::mach::invalid_name;
    if ((entry->type & type_mask(Right::Receive)) == 0)
        return darwin::mach::invalid_right;
    const auto port = state.mach_port_objects.lookup(entry->object);
    if (!port)
        return darwin::mach::invalid_right;
    if (operation == Operation::Guard) { // mach_port_guard
        if (state.mach_port_contexts[entry->object] != 0)
            return darwin::mach::invalid_argument;
        static_cast<void>(
            state.mach_port_objects.set_guard(entry->object, guard, strict));
        state.mach_port_contexts[entry->object] = guard;
        return darwin::mach::success;
    }
    if ((port->guard && *port->guard != guard) ||
        (operation == Operation::Unguard && !port->guard))
        return darwin::mach::invalid_argument;
    if (operation == Operation::Unguard) { // mach_port_unguard
        static_cast<void>(
            state.mach_port_objects.set_guard(entry->object, std::nullopt));
        state.mach_port_contexts[entry->object] = 0;
        return darwin::mach::success;
    }

    // Destruct drops the receive right and only the requested send references.
    // The common right-lifetime path handles queued rights and notifications.
    if (delta > 0)
        return darwin::mach::invalid_value;
    if (delta != 0) {
        const auto result = modify_port_references_locked(
            state, task, name, Right::Send, delta);
        if (result != darwin::mach::success)
            return result;
    }
    return modify_port_references_locked(state, task, name, Right::Receive, -1);
}

std::uint32_t dispatch_guarded_port_trap(KernelSharedState& state,
    AddressSpace& memory, std::uint32_t task,
    const std::array<std::uint32_t, 16>& registers, std::uint32_t trap)
{
    const auto wide = [&](std::size_t index) {
        return static_cast<std::uint64_t>(registers[index]) |
               (static_cast<std::uint64_t>(registers[index + 1]) << 32U);
    };
    if (trap == 24U) {
        std::array<std::byte, GuardedPorts::options_size> options;
        // XNU copies the entire options structure before deciding whether the
        // trap must fall back to a remote-task MIG call.
        if (!memory.copy_out(registers[1], options))
            return darwin::mach_message::send_invalid_data;
        if (task == 0U)
            return darwin::mach_message::send_invalid_destination;
        const auto result = GuardedPorts::construct_locked(state, task,
            { read_little_word(options, 0U), read_little_word(options, 4U) },
            wide(2));
        if (result.error != darwin::mach::success)
            return result.error;
        // Native copyout faults do not undo a successfully constructed right.
        return memory.write32(registers[4], result.name)
                   ? darwin::mach::success
                   : darwin::error::bad_address;
    }
    if (task == 0U)
        return darwin::mach_message::send_invalid_destination;
    const auto operation = trap == 41U   ? GuardedPorts::Operation::Guard
                           : trap == 42U ? GuardedPorts::Operation::Unguard
                                         : GuardedPorts::Operation::Destruct;
    return GuardedPorts::change_locked(state, task, registers[1], operation,
        wide(trap == 25U ? 3U : 2U), std::bit_cast<std::int32_t>(registers[2]),
        registers[4] != 0);
}

} // namespace ilemu
