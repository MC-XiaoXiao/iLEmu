// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// XNU792--4903 mach_port_{move,insert,extract}_member MIG transport.
#include "membership.hpp"
#include "../transport/kernel_reply.hpp"

namespace ilemu::port_mig {
using namespace mach_support;
using namespace xnu::mig::mach_port;

std::uint32_t Membership::evaluate_locked(KernelSharedState& state,
    std::uint32_t object, std::span<const std::byte> bytes)
{
    if (bytes.size() != 40U ||
        (read_little_word(bytes, 0U) & darwin::mig_wire::message_complex_bit) != 0U)
        return darwin::mig::bad_arguments;
    const auto target = state.task_port_pids.find(object);
    if (target == state.task_port_pids.end())
        return darwin::mach::invalid_task;
    const auto process = state.processes.find(target->second);
    if (process == state.processes.end() || process->second.exited ||
        !state.mach_namespaces.contains_task(target->second))
        return darwin::mach::invalid_task;
    const auto identifier = read_little_word(bytes, 20U);
    const auto operation = identifier == id(Routine::mach_port_move_member)
        ? PortMembershipOperation::Move
        : identifier == id(Routine::mach_port_insert_member)
        ? PortMembershipOperation::Insert : PortMembershipOperation::Extract;
    return modify_port_membership_locked(state, target->second,
        read_little_word(bytes, 32U), read_little_word(bytes, 36U), operation).result;
}

std::optional<std::uint32_t> Membership::dispatch_locked(
    KernelSharedState& state, std::uint32_t object,
    KernelSharedState::MachMessage& request)
{
    const auto result = evaluate_locked(state, object, request.bytes);
    return mach_ipc::enqueue_kernel_reply_locked(state, request,
        read_little_word(request.bytes, 20U), std::array { 0U, 1U, result });
}

std::optional<Membership::SynchronousReply> Membership::try_synchronous_locked(
    AddressSpace& memory, KernelSharedState& state,
    const ProcessContext& process, std::span<const std::uint32_t> registers,
    std::uint32_t bits, std::uint32_t reply_name, std::uint32_t receive_address)
{
    const auto destination = mach_ipc::validate_task_rpc_locked(memory, state,
        process, registers, bits, reply_name, 40U, 44U);
    if (!destination)
        return std::nullopt;
    std::array<std::byte, 40> bytes;
    if (!memory.copy_out(registers[0], bytes) || read_little_word(bytes, 16U) != 0U)
        return std::nullopt;
    state.mach_port_objects.make_send_once(destination->reply_object);
    const auto result = evaluate_locked(state, destination->task_object, bytes);
    const std::array payload { 0U, 1U, result };
    // The operation may attach the reply port to a set with waiting receivers.
    // Recheck its links after evaluation and preserve normal ready-queue order.
    const auto queue = state.mach_queues.find(destination->reply_object);
    if (queue != state.mach_queues.end() && queue->second.empty() &&
        !state.mach_port_set_links_by_member.contains(destination->reply_object) &&
        memory.accessible(receive_address, 44U, MemoryPermission::Write)) {
        return SynchronousReply { mach_ipc::copyout_kernel_reply_locked<3>(
            memory, state, receive_address, reply_name, destination->reply_object,
            read_little_word(bytes, 20U), payload) };
    }
    KernelSharedState::MachMessage request;
    request.bytes.assign(bytes.begin(), bytes.end());
    request.reply_object = destination->reply_object;
    request.reply_right = xnu::ipc::Right::SendOnce;
    static_cast<void>(mach_ipc::enqueue_kernel_reply_locked(
        state, request, read_little_word(bytes, 20U), payload));
    return SynchronousReply { };
}
} // namespace ilemu::port_mig
