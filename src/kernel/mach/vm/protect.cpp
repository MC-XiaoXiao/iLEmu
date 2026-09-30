// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
#include "protect.hpp"
#include "wire_format.hpp"
#include "kernel/kernel.hpp"
#include <algorithm>
#include <limits>

namespace ilemu {
namespace vm_mig {
using namespace mach_support;

std::uint32_t Protection::execute(AddressSpace& memory, Cpu& cpu,
    std::uint64_t address, std::uint64_t size, bool wide,
    bool set_maximum, std::uint32_t protection)
{
    // vm_user.c validates in the endpoint's integer width before zero-size.
    constexpr std::uint32_t copy = 0x10U;
    const auto maximum = wide ? UINT64_MAX : std::uint64_t { UINT32_MAX };
    if (address > maximum || size > maximum - address ||
        (protection & ~(7U | copy)) != 0U)
        return darwin::mach::invalid_argument;
    if (size == 0U)
        return darwin::mach::success;
    constexpr auto limit = std::uint64_t { 1 } << 32U;
    if (address >= limit || size > limit - address)
        return darwin::mach::invalid_address;
    const auto result = memory.protect_with_result(
        static_cast<std::uint32_t>(address),
        static_cast<std::uint32_t>(std::min(size, std::uint64_t { UINT32_MAX })),
        static_cast<MemoryPermission>(protection & 7U), set_maximum,
        (protection & copy) != 0U);
    if (!result.succeeded)
        return result.protection_failure ? 2U : darwin::mach::invalid_address;
    if (result.executable_permissions_changed) {
        constexpr auto mask = std::uint64_t { AddressSpace::page_size - 1U };
        const auto first = address & ~mask;
        const auto end = (address + size + mask) & ~mask;
        cpu.invalidate_cache_range(static_cast<std::uint32_t>(first),
            static_cast<std::size_t>(end - first));
    }
    return darwin::mach::success;
}

std::uint32_t Protection::evaluate_locked(AddressSpace& memory, Cpu& cpu,
    const KernelSharedState& state, std::uint32_t caller,
    std::uint32_t object, std::span<const std::byte> bytes)
{
    const auto width = mach_vm_support::MachVmWireFormat::for_interface(
        read_little_word(bytes, 20U) == 4802U,
        state.darwin_abi.mach_vm_address).address_size();
    if (bytes.size() != 40U + 2U * width ||
        (read_little_word(bytes, 0U) & darwin::mig_wire::message_complex_bit))
        return darwin::mig::bad_arguments;
    const auto target = state.task_port_pids.find(object);
    if (target == state.task_port_pids.end() || target->second != caller)
        return darwin::mach::invalid_argument;
    const auto read_address = [&](std::size_t offset) -> std::uint64_t {
        return read_little_word(bytes, offset) | (width == 8U
            ? std::uint64_t { read_little_word(bytes, offset + 4U) } << 32U : 0U);
    };
    return execute(memory, cpu, read_address(32U),
        read_address(32U + width), width == 8U,
        read_little_word(bytes, 32U + 2U * width) != 0U,
        read_little_word(bytes, 36U + 2U * width));
}

std::optional<std::uint32_t> Protection::dispatch_locked(AddressSpace& memory,
    Cpu& cpu, KernelSharedState& state, std::uint32_t caller,
    std::uint32_t object, KernelSharedState::MachMessage& request)
{
    const auto identifier = read_little_word(request.bytes, 20U);
    const auto result = evaluate_locked(memory, cpu, state, caller, object, request.bytes);
    return mach_ipc::enqueue_kernel_reply_locked(
        state, request, identifier, std::array { 0U, 1U, result });
}

std::optional<std::uint32_t> Protection::try_synchronous_locked(AddressSpace& memory,
    Cpu& cpu, KernelSharedState& state, const ProcessContext& process,
    std::span<const std::uint32_t> registers, std::uint32_t bits,
    std::uint32_t reply_name, std::uint32_t receive_address, std::uint32_t identifier)
{
    const auto width = mach_vm_support::MachVmWireFormat::for_interface(
        identifier == 4802U, state.darwin_abi.mach_vm_address).address_size();
    const auto request_size = 40U + 2U * width;
    constexpr auto reply_with_trailer = 44U;
    const auto destination = mach_ipc::validate_task_rpc_locked(memory, state,
        process, registers, bits, reply_name, request_size, reply_with_trailer);
    if (!destination || !memory.accessible(
            receive_address, reply_with_trailer, MemoryPermission::Write))
        return std::nullopt;
    std::array<std::byte, 56> storage;
    const auto bytes = std::span { storage }.first(request_size);
    if (!memory.copy_out(registers[0], bytes))
        return darwin::mach_message::send_invalid_data;
    if (read_little_word(bytes, 16U) != 0U)
        return std::nullopt;
    // Protection can make the reply buffer unwritable. Once it executes,
    // return copyout's error without dispatching the operation a second time.
    const auto result = evaluate_locked(
        memory, cpu, state, process.pid, destination->task_object, bytes);
    state.mach_port_objects.make_send_once(destination->reply_object);
    return mach_ipc::copyout_kernel_reply_locked<3>(memory, state, receive_address,
        reply_name, destination->reply_object, identifier, std::array { 0U, 1U, result });
}
} // namespace vm_mig
} // namespace ilemu
