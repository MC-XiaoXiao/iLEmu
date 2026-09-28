// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// XNU792--4903 mach_port_{get,set}_attributes and their MIG wire contracts.
#include "attributes.hpp"
#include "../transport/kernel_reply.hpp"

namespace ilemu::port_mig {
using namespace mach_support;
using namespace xnu::mig::mach_port;

Attributes::Result Attributes::evaluate_locked(KernelSharedState& state,
    std::uint32_t object, std::span<const std::byte> bytes)
{
    const bool get = read_little_word(bytes, 20U) == id(Routine::mach_port_get_attributes);
    const auto maximum_count = state.darwin_abi.mach_port_attribute_array ==
        DarwinMachPortAttributeArray::ExtendedStatus17 ? 17U : 10U;
    const auto count = read_little_word(bytes, 40U);
    if ((read_little_word(bytes, 0U) & darwin::mig_wire::message_complex_bit) != 0U ||
        bytes.size() < 44U || (get ? bytes.size() != 44U :
            count > maximum_count || bytes.size() != 44U + count * 4U))
        return { darwin::mig::bad_arguments };
    const auto target = state.task_port_pids.find(object);
    if (target == state.task_port_pids.end())
        return { darwin::mach::invalid_task };
    const auto task = target->second;
    const auto process = state.processes.find(task);
    if (process == state.processes.end() || process->second.exited ||
        !state.mach_namespaces.contains_task(task))
        return { darwin::mach::invalid_task };
    const auto name = read_little_word(bytes, 32U);
    const auto flavor = read_little_word(bytes, 36U);
    const auto value = read_little_word(bytes, 44U);
    if (get ? flavor != 1U && flavor != 2U : flavor != 1U && flavor != 4U)
        return { darwin::mach::invalid_argument };
    const auto required = flavor == 1U ? 1U : get ? 10U : 0U;
    if (count < required)
        return { darwin::mach::failure };
    if (!get && flavor == 1U && value > xnu::ipc::maximum_queue_limit)
        return { darwin::mach::invalid_value };
    Result result;
    if (name == xnu::ipc::null_name || name == xnu::ipc::dead_name) {
        if (!get || flavor != 1U)
            return { darwin::mach::invalid_right };
        result.count = 4U; // LIMITS on NULL/DEAD succeeds with output count0.
        return result;
    }
    const auto entry = state.mach_namespaces.lookup(task, name);
    if (!entry)
        return { darwin::mach::invalid_name };
    if ((entry->type & xnu::ipc::type_mask(xnu::ipc::Right::Receive)) == 0U)
        return { darwin::mach::invalid_right };
    const auto port = state.mach_port_objects.lookup(entry->object);
    if (!port)
        return { darwin::mach::invalid_right };
    if (!get) {
        if (flavor == 4U) {
            if (!state.mach_port_objects.set_temporary_owner(entry->object))
                return { darwin::mach::invalid_argument };
        } else {
            static_cast<void>(state.mach_port_objects.set_queue_limit(entry->object, value));
            if (value > port->queue_limit)
                state.grant_mach_send_slots_locked(entry->object, value - port->queue_limit);
            state.notify_send_possible_locked(entry->object);
        }
        return result;
    }
    result.words[3] = required;
    result.count = 4U + required;
    if (flavor == 1U) {
        result.words[4] = port->queue_limit;
        return result;
    }
    const auto links = state.mach_port_set_links_by_member.find(entry->object);
    result.words[4] = links == state.mach_port_set_links_by_member.end() ? 0U :
        static_cast<std::uint32_t>(links->second.size());
    result.words[5] = port->sequence_number;
    result.words[6] = port->make_send_count;
    result.words[7] = port->queue_limit;
    result.words[8] = static_cast<std::uint32_t>(state.mach_message_count_locked(entry->object));
    // Send-once accounting and importance flags retain their existing empty
    // state until their full ownership lifetimes are modeled.
    result.words[10] = port_has_send_rights_locked(state, entry->object) ? 1U : 0U;
    result.words[11] = state.mach_notifications.contains(
        { entry->object, mach_notify_port_destroyed }) ? 1U : 0U;
    result.words[12] = state.mach_notifications.contains(
        { entry->object, mach_notify_no_senders }) ? 1U : 0U;
    return result;
}

std::optional<std::uint32_t> Attributes::dispatch_locked(
    KernelSharedState& state, std::uint32_t object,
    KernelSharedState::MachMessage& request)
{
    const auto identifier = read_little_word(request.bytes, 20U);
    const auto result = evaluate_locked(state, object, request.bytes);
    return mach_ipc::enqueue_kernel_reply_locked(
        state, request, identifier, result.payload());
}

std::optional<Attributes::SynchronousReply> Attributes::try_synchronous_locked(
    AddressSpace& memory, KernelSharedState& state,
    const ProcessContext& process, std::span<const std::uint32_t> registers,
    std::uint32_t bits, std::uint32_t reply_name, std::uint32_t receive_address)
{
    // The largest native inline input is17 words; most calls need only44/48
    // bytes. Keep uncontended requests and replies on the stack.
    constexpr std::uint32_t maximum_request_size = 44U + 17U * 4U;
    const auto size = registers[2];
    if (size < 44U || size > maximum_request_size)
        return std::nullopt;
    const auto identifier = memory.read32(registers[0] + 20U).value_or(0U);
    const auto flavor = memory.read32(registers[0] + 36U).value_or(0U);
    const auto capacity = identifier == id(Routine::mach_port_get_attributes)
        ? (flavor == 2U ? 88U : 52U) : 44U;
    const auto destination = mach_ipc::validate_task_rpc_locked(memory, state,
        process, registers, bits, reply_name, size, capacity);
    if (!destination)
        return std::nullopt;
    std::array<std::byte, maximum_request_size> storage;
    const auto bytes = std::span { storage }.first(size);
    if (!memory.copy_out(registers[0], bytes) || read_little_word(bytes, 16U) != 0U)
        return std::nullopt;
    const auto result = evaluate_locked(state, destination->task_object, bytes);
    // Raising a limit may wake blocked senders or produce SEND_POSSIBLE on
    // this reply port. Preserve their order by falling back to its queue.
    const auto queue = state.mach_queues.find(destination->reply_object);
    const auto output_size = 32U + static_cast<std::uint32_t>(result.payload().size_bytes());
    if (queue != state.mach_queues.end() && queue->second.empty() &&
        memory.accessible(receive_address, output_size, MemoryPermission::Write)) {
        return SynchronousReply { mach_ipc::copyout_kernel_reply_locked<14>(
            memory, state, receive_address, reply_name, destination->reply_object,
            identifier, result.payload()) };
    }
    KernelSharedState::MachMessage request;
    request.bytes.assign(bytes.begin(), bytes.end());
    request.reply_object = destination->reply_object;
    request.reply_right = xnu::ipc::Right::SendOnce;
    static_cast<void>(mach_ipc::enqueue_kernel_reply_locked(
        state, request, identifier, result.payload()));
    return SynchronousReply { };
}
} // namespace ilemu::port_mig
