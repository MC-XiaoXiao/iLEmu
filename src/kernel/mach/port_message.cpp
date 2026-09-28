// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "port/lifecycle.hpp"
#include "transport/kernel_reply.hpp"
#include <kernel/kernel.hpp>

namespace ilemu {
bool CompatibilityKernel::dispatch_mach_port_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    return dispatch_mach_port_membership_message(cpu, request);
}
} // namespace ilemu

namespace ilemu::port_mig {
using namespace mach_support;
using namespace xnu::mig::mach_port;

bool Lifecycle::handles(std::uint32_t identifier)
{
    switch (static_cast<Routine>(identifier)) {
    case Routine::mach_port_rename:
    case Routine::mach_port_allocate_name:
    case Routine::mach_port_allocate:
    case Routine::mach_port_destroy:
    case Routine::mach_port_deallocate:
    case Routine::mach_port_mod_refs:
    case Routine::mach_port_set_mscount:
    case Routine::mach_port_set_seqno:
        return true;
    default:
        return false;
    }
}

std::uint32_t Lifecycle::request_size(std::uint32_t identifier)
{
    switch (static_cast<Routine>(identifier)) {
    case Routine::mach_port_allocate:
    case Routine::mach_port_destroy:
    case Routine::mach_port_deallocate:
        return 36U;
    case Routine::mach_port_mod_refs:
        return 44U;
    default:
        return 40U;
    }
}

Lifecycle::Result Lifecycle::allocate_locked(KernelSharedState& state,
    std::uint32_t task, std::uint32_t right, std::optional<std::uint32_t> name)
{
    if (right != 1U && right != 3U && right != 4U)
        return { darwin::mach::invalid_value };
    // XNU792 through4903 allocate_full uses ipc_object_alloc_dead even when
    // qos.name is set: the explicit name is ignored for a dead-name right.
    if (right == 4U)
        name.reset();
    if (name && state.mach_namespaces.contains(task, *name))
        return { darwin::mach::name_exists };
    const auto object = state.allocate_mach_object();
    const auto type = 1U << (right + 16U);
    if (name) {
        if (!state.mach_namespaces.install(task, *name, object, type))
            return { darwin::mach::resource_shortage };
    } else {
        name = state.mach_namespaces.allocate(task, object, type);
        if (!name)
            return { darwin::mach::no_space };
    }
    if (right == 1U) {
        static_cast<void>(state.mach_port_objects.create(object, task));
        state.mach_queues.try_emplace(object);
    } else if (right == 3U) {
        static_cast<void>(state.create_mach_port_set_locked(object));
    }
    return { 0U, *name };
}

std::uint32_t Lifecycle::deallocate_locked(
    KernelSharedState& state, std::uint32_t task, std::uint32_t name)
{
    if (name == xnu::ipc::null_name || name == xnu::ipc::dead_name)
        return 0U;
    const auto entry = state.mach_namespaces.lookup(task, name);
    if (!entry)
        return darwin::mach::invalid_name;
    for (const auto right : { xnu::ipc::Right::Send, xnu::ipc::Right::SendOnce,
             xnu::ipc::Right::DeadName }) {
        if ((entry->type & xnu::ipc::type_mask(right)) != 0U)
            return modify_port_references_locked(state, task, name, right, -1);
    }
    return darwin::mach::invalid_right;
}

Lifecycle::Result Lifecycle::evaluate_locked(KernelSharedState& state,
    std::uint32_t object, std::span<const std::byte> bytes)
{
    const auto identifier = read_little_word(bytes, 20U);
    if (bytes.size() != request_size(identifier) ||
        (read_little_word(bytes, 0U) & darwin::mig_wire::message_complex_bit))
        return { darwin::mig::bad_arguments };
    const auto routine = static_cast<Routine>(identifier);
    const auto name = read_little_word(bytes, 32U);
    const auto value = read_little_word(bytes, 36U);
    const auto valid = [](std::uint32_t n) {
        return n != xnu::ipc::null_name && n != xnu::ipc::dead_name;
    };
    // The allocate_name wrapper validates its scalar name before allocating.
    if (routine == Routine::mach_port_allocate_name && !valid(value))
        return { darwin::mach::invalid_value };
    const auto target = state.task_port_pids.find(object);
    if (target == state.task_port_pids.end())
        return { darwin::mach::invalid_task };
    const auto task = target->second;
    const auto process = state.processes.find(task);
    if (process == state.processes.end() || process->second.exited ||
        !state.mach_namespaces.contains_task(task))
        return { darwin::mach::invalid_task };
    switch (routine) {
    case Routine::mach_port_allocate:
        return allocate_locked(state, task, name);
    case Routine::mach_port_allocate_name:
        return allocate_locked(state, task, name, value);
    case Routine::mach_port_destroy:
        return { !valid(name) || destroy_port_name_locked(state, task, name)
                     ? 0U
                     : darwin::mach::invalid_name };
    case Routine::mach_port_deallocate:
        return { deallocate_locked(state, task, name) };
    case Routine::mach_port_mod_refs:
        if (value > static_cast<std::uint32_t>(xnu::ipc::Right::DeadName))
            return { darwin::mach::invalid_value };
        return { modify_port_references_locked(state, task, name,
            static_cast<xnu::ipc::Right>(value),
            static_cast<std::int32_t>(read_little_word(bytes, 40U))) };
    case Routine::mach_port_set_seqno:
    case Routine::mach_port_set_mscount: {
        if (!valid(name))
            return { darwin::mach::invalid_right };
        const auto entry = state.mach_namespaces.lookup(task, name);
        if (!entry)
            return { darwin::mach::invalid_name };
        if ((entry->type & xnu::ipc::type_mask(xnu::ipc::Right::Receive)) == 0U)
            return { darwin::mach::invalid_right };
        if (routine == Routine::mach_port_set_seqno)
            static_cast<void>(
                state.mach_port_objects.set_sequence_number(entry->object, value));
        else
            static_cast<void>(
                state.mach_port_objects.set_make_send_count(entry->object, value));
        return { };
    }
    case Routine::mach_port_rename: {
        if (!valid(name))
            return { darwin::mach::invalid_name };
        if (!valid(value))
            return { darwin::mach::invalid_value };
        // ipc_object_rename allocates the destination before looking up old.
        if (state.mach_namespaces.contains(task, value))
            return { darwin::mach::name_exists };
        if (!state.mach_namespaces.contains(task, name))
            return { darwin::mach::invalid_name };
        if (!state.mach_namespaces.rename(task, name, value))
            return { darwin::mach::resource_shortage };
        auto notification =
            state.mach_dead_name_notifications.extract({ task, name });
        if (!notification.empty()) {
            notification.key().second = value;
            state.mach_dead_name_notifications.insert(std::move(notification));
        }
        return { };
    }
    default:
        return { darwin::mig::bad_id };
    }
}

std::optional<std::uint32_t> Lifecycle::reply_locked(KernelSharedState& state,
    KernelSharedState::MachMessage& request, Result result)
{
    const auto identifier = read_little_word(request.bytes, 20U);
    const std::array<std::uint32_t, 4> payload { 0U, 1U, result.error,
        result.name };
    return mach_ipc::enqueue_kernel_reply_locked(state, request, identifier,
        std::span { payload }.first(
            result.error == 0U && identifier == id(Routine::mach_port_allocate)
                ? 4U
                : 3U));
}

std::optional<std::uint32_t> Lifecycle::dispatch_locked(
    KernelSharedState& state, std::uint32_t object,
    KernelSharedState::MachMessage& request)
{
    return reply_locked(
        state, request, evaluate_locked(state, object, request.bytes));
}

std::optional<Lifecycle::SynchronousReply> Lifecycle::try_synchronous_locked(
    AddressSpace& memory, KernelSharedState& state,
    const ProcessContext& process, std::span<const std::uint32_t> registers,
    std::uint32_t bits, std::uint32_t reply_name, std::uint32_t receive_address)
{
    const auto identifier = memory.read32(registers[0] + 20U).value_or(0U);
    const auto capacity =
        identifier == id(Routine::mach_port_allocate) ? 48U : 44U;
    const auto size = request_size(identifier);
    const auto destination = mach_ipc::validate_task_rpc_locked(
        memory, state, process, registers, bits, reply_name, size, capacity);
    if (!destination)
        return std::nullopt;
    constexpr auto maximum_request_size =
        mach_port_mod_refs_arguments.back().request_offset +
        sizeof(std::uint32_t);
    std::array<std::byte, maximum_request_size> storage;
    const auto bytes = std::span { storage }.first(size);
    if (!memory.copy_out(registers[0], bytes) ||
        read_little_word(bytes, 16U) != 0U)
        return std::nullopt;
    const auto result = evaluate_locked(state, destination->task_object, bytes);
    // Mutation can destroy or rename the reply right, or enqueue a
    // notification ahead of the reply. Preserve ordinary receive then.
    const auto reply_object = destination->reply_object;
    const auto queue = state.mach_queues.find(reply_object);
    if (resolve_name_with_right(state, process.pid, reply_name,
            xnu::ipc::Right::Receive) == reply_object &&
        state.mach_port_objects.contains(reply_object) &&
        queue != state.mach_queues.end() && queue->second.empty() &&
        memory.accessible(receive_address, capacity, MemoryPermission::Write)) {
        const std::array<std::uint32_t, 4> payload { 0U, 1U, result.error,
            result.name };
        return SynchronousReply { mach_ipc::copyout_kernel_reply_locked(memory,
            state, receive_address, reply_name, reply_object, identifier,
            std::span { payload }.first(
                result.error == 0U &&
                        identifier == id(Routine::mach_port_allocate)
                    ? 4U
                    : 3U)) };
    }
    KernelSharedState::MachMessage request;
    request.bytes.assign(bytes.begin(), bytes.end());
    request.reply_object = reply_object;
    request.reply_right = xnu::ipc::Right::SendOnce;
    static_cast<void>(reply_locked(state, request, result));
    return SynchronousReply { };
}
} // namespace ilemu::port_mig
