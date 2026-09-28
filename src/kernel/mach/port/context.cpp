// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Native mach_port_{get,set}_context MIG transport and object-owned context.
// References: XNU1504--4903 osfmk/ipc/mach_port.c, mach_port.defs.
#include "context.hpp"
#include "../transport/kernel_reply.hpp"
#include "../vm/wire_format.hpp"

namespace ilemu::port_mig {
using namespace mach_support;

Context::Result Context::evaluate_locked(KernelSharedState& state,
    std::uint32_t object, std::span<const std::byte> bytes)
{
    const bool get = read_little_word(bytes, 20U) == get_identifier;
    const auto width = mach_vm_support::MachVmWireFormat::for_interface(
        true, state.darwin_abi.mach_port_context).address_size();
    if (bytes.size() != (get ? 36U : 36U + width) ||
        (read_little_word(bytes, 0U) & darwin::mig_wire::message_complex_bit) != 0U)
        return Result { darwin::mig::bad_arguments };
    const auto target = state.task_port_pids.find(object);
    if (target == state.task_port_pids.end())
        return Result { darwin::mach::invalid_task };
    const auto process = state.processes.find(target->second);
    if (process == state.processes.end() || process->second.exited ||
        !state.mach_namespaces.contains_task(target->second))
        return Result { darwin::mach::invalid_task };
    const auto name = read_little_word(bytes, 32U);
    if (name == xnu::ipc::null_name || name == xnu::ipc::dead_name)
        return Result { darwin::mach::invalid_right };
    const auto entry = state.mach_namespaces.lookup(target->second, name);
    if (!entry)
        return Result { darwin::mach::invalid_name };
    if ((entry->type & xnu::ipc::type_mask(xnu::ipc::Right::Receive)) == 0U)
        return Result { darwin::mach::invalid_right };
    const auto port = state.mach_port_objects.lookup(entry->object);
    if (!port)
        return Result { darwin::mach::invalid_right };
    if (get) {
        Result result;
        result.count += width / 4U;
        const auto context = state.mach_port_contexts.find(entry->object);
        if (!port->strict_guard && context != state.mach_port_contexts.end()) {
            result.words[3] = static_cast<std::uint32_t>(context->second);
            if (width == 8U)
                result.words[4] = static_cast<std::uint32_t>(context->second >> 32U);
        }
        return result;
    }
    if (port->strict_guard)
        return Result { darwin::mach::invalid_argument };
    const auto context = static_cast<std::uint64_t>(read_little_word(bytes, 36U)) |
        (width == 8U ? static_cast<std::uint64_t>(read_little_word(bytes, 40U)) << 32U : 0U);
    state.mach_port_contexts[entry->object] = context;
    if (port->guard)
        static_cast<void>(state.mach_port_objects.set_guard(entry->object, context));
    return Result { };
}

std::optional<std::uint32_t> Context::dispatch_locked(
    KernelSharedState& state, std::uint32_t object,
    KernelSharedState::MachMessage& request)
{
    const auto result = evaluate_locked(state, object, request.bytes);
    return mach_ipc::enqueue_kernel_reply_locked(state, request,
        read_little_word(request.bytes, 20U), result.payload());
}

std::optional<std::uint32_t> Context::try_synchronous_locked(
    AddressSpace& memory, KernelSharedState& state,
    const ProcessContext& process, std::span<const std::uint32_t> registers,
    std::uint32_t bits, std::uint32_t reply_name, std::uint32_t receive_address)
{
    const auto identifier = memory.read32(registers[0] + 20U).value_or(0U);
    const bool get = identifier == get_identifier;
    const auto width = mach_vm_support::MachVmWireFormat::for_interface(
        true, state.darwin_abi.mach_port_context).address_size();
    const auto request_size = get ? 36U : 36U + width;
    const auto reply_capacity = get ? 44U + width : 44U;
    const auto destination = mach_ipc::validate_task_rpc_locked(memory, state,
        process, registers, bits, reply_name, request_size, reply_capacity);
    if (!destination ||
        !memory.accessible(receive_address, reply_capacity, MemoryPermission::Write))
        return std::nullopt;
    std::array<std::byte, 44> storage;
    const auto bytes = std::span { storage }.first(request_size);
    if (!memory.copy_out(registers[0], bytes) || read_little_word(bytes, 16U) != 0U)
        return std::nullopt;
    // No state changes precede the last fallback check. Non-default headers,
    // trailers, small/read-only output or queued replies use normal IPC.
    state.mach_port_objects.make_send_once(destination->reply_object);
    const auto result = evaluate_locked(state, destination->task_object, bytes);
    return mach_ipc::copyout_kernel_reply_locked<5>(memory, state, receive_address,
        reply_name, destination->reply_object, identifier, result.payload());
}
} // namespace ilemu::port_mig
