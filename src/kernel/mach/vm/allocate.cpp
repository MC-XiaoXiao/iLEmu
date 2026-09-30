// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
#include "allocate.hpp"
#include "wire_format.hpp"

namespace ilemu::vm_mig {
using namespace mach_support;

// vm_map retains natural fields; mach_vm follows the configured wire width.
// References: vm_map.defs; mach_vm.defs; vm_user.c; ipc_tt.c:convert_port_to_map.
Allocation::Result Allocation::evaluate_locked(AddressSpace& memory,
    const KernelSharedState& state, std::uint32_t caller, std::uint32_t object,
    std::span<const std::byte> bytes)
{
    const auto width = mach_vm_support::MachVmWireFormat::for_interface(
        read_little_word(bytes, 20U) == 4800U,
        state.darwin_abi.mach_vm_address).address_size();
    if (bytes.size() != 36U + 2U * width ||
        (read_little_word(bytes, 0U) & darwin::mig_wire::message_complex_bit))
        return Result { darwin::mig::bad_arguments };
    const auto target = state.task_port_pids.find(object);
    // Target address-space mutation is currently available for the calling
    // task. A different or non-task object must not allocate in this map.
    if (target == state.task_port_pids.end() || target->second != caller)
        return Result { darwin::mach::invalid_argument };
    const auto read_address = [&](std::size_t offset) -> std::uint64_t {
        return read_little_word(bytes, offset) | (width == 8U
            ? std::uint64_t { read_little_word(bytes, offset + 4U) } << 32U : 0U);
    };
    const auto address = read_address(32U);
    const auto size = read_address(32U + width);
    const auto flags = read_little_word(bytes, 32U + 2U * width);
    const auto allocation = allocate_guest_vm_region(memory,
        darwin::mach::vm_allocation::Contract { state.darwin_abi.abi_epoch },
        address, size, flags);
    Result result { allocation.result };
    result.address_words = width / 4U;
    result.words[3] = allocation.address;
    return result;
}

std::optional<std::uint32_t> Allocation::dispatch_locked(AddressSpace& memory,
    KernelSharedState& state, std::uint32_t caller, std::uint32_t object,
    KernelSharedState::MachMessage& request)
{
    const auto identifier = read_little_word(request.bytes, 20U);
    const auto result = evaluate_locked(memory, state, caller, object, request.bytes);
    return mach_ipc::enqueue_kernel_reply_locked(
        state, request, identifier, result.payload());
}

std::optional<std::uint32_t> Allocation::try_synchronous_locked(
    AddressSpace& memory, KernelSharedState& state, const ProcessContext& process,
    std::span<const std::uint32_t> registers, std::uint32_t bits,
    std::uint32_t reply_name, std::uint32_t receive_address, std::uint32_t identifier)
{
    const auto width = mach_vm_support::MachVmWireFormat::for_interface(
        identifier == 4800U, state.darwin_abi.mach_vm_address).address_size();
    const auto request_size = 36U + 2U * width;
    const auto reply_with_trailer = 44U + width;
    const auto destination = mach_ipc::validate_task_rpc_locked(memory, state,
        process, registers, bits, reply_name, request_size, reply_with_trailer);
    if (!destination || !memory.accessible(
            receive_address, reply_with_trailer, MemoryPermission::Write))
        return std::nullopt;
    std::array<std::byte, 52> storage;
    const auto bytes = std::span { storage }.first(request_size);
    if (!memory.copy_out(registers[0], bytes))
        return darwin::mach_message::send_invalid_data;
    if (read_little_word(bytes, 16U) != 0U)
        return std::nullopt;
    // No fallback is possible after allocation: a short/unwritable receive
    // buffer uses queued IPC from the outset and must not allocate twice.
    const auto result = evaluate_locked(
        memory, state, process.pid, destination->task_object, bytes);
    state.mach_port_objects.make_send_once(destination->reply_object);
    return mach_ipc::copyout_kernel_reply_locked<5>(memory, state,
        receive_address, reply_name, destination->reply_object,
        identifier, result.payload());
}
} // namespace ilemu::vm_mig
