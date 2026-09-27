// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// XNU task_threads returns an OOL array of newly made thread send rights.
// Copyout belongs to the actual receiver, not the task issuing the request.

#include "enumeration.hpp"
#include "../transport/kernel_reply.hpp"

#include <array>
#include <vector>

namespace ilemu::task_mig {
bool Enumeration::try_synchronous_enqueue_locked(AddressSpace& memory,
    KernelSharedState& state, const ProcessContext& process,
    std::span<const std::uint32_t> registers, std::uint32_t bits,
    std::uint32_t reply_name)
{
    using namespace mach_support;
    constexpr auto send_receive = darwin::mach_message::option_send |
                                  darwin::mach_message::option_receive;
    constexpr auto options = send_receive |
                             darwin::mach_message::option_send_timeout |
                             darwin::mach_message::option_receive_timeout |
                             darwin::mach_message::option_receive_large;
    if (bits != (darwin::mig_wire::disposition_copy_send |
                    (darwin::mig_wire::disposition_make_send_once << 8U)) ||
        (registers[1] & send_receive) != send_receive ||
        (registers[1] & ~options) != 0U || registers[2] != 24U ||
        registers[3] < 60U || registers[4] != reply_name)
        return false;
    const auto target = memory.read32(registers[0] + 8U);
    const auto object = target ? resolve_name_with_right(
        state, process.pid, *target, xnu::ipc::Right::Send) : std::nullopt;
    if (!object || !state.task_port_pids.contains(*object))
        return false;
    const auto reply = resolve_name_with_right(
        state, process.pid, reply_name, xnu::ipc::Right::Receive);
    if (!reply || !state.mach_port_objects.contains(*reply) ||
        state.mach_port_set_links_by_member.contains(*reply))
        return false;
    const auto queue = state.mach_queues.find(*reply);
    if (queue == state.mach_queues.end() || !queue->second.empty())
        return false;
    KernelSharedState::MachMessage request;
    request.bytes.resize(24U);
    if (!memory.copy_out(registers[0], request.bytes))
        return false;
    // COPY_SEND never consumes the task right. MAKE_SEND_ONCE is produced
    // directly in transit under mach_mutex, as in ipc_kobject_server.
    // Keep the shared queue/receiver responsible for OOL names, VM and faults.
    request.reply_object = reply;
    request.reply_right = xnu::ipc::Right::SendOnce;
    return dispatch_locked(state, *object, request).has_value();
}

std::optional<std::uint32_t> Enumeration::dispatch_locked(KernelSharedState& state,
    std::uint32_t object, KernelSharedState::MachMessage& request)
{
    using namespace mach_support;
    constexpr auto identifier =
        xnu::mig::task::id(xnu::mig::task::Routine::task_threads);
    const auto error = [&](std::uint32_t result) {
        const std::array<std::uint32_t, 3> payload { 0U, 1U, result };
        return mach_ipc::enqueue_kernel_reply_locked(
            state, request, identifier, payload);
    };
    if (request.bytes.size() != darwin::mig_wire::message_header_size ||
        (read_little_word(request.bytes, 0U) &
            darwin::mig_wire::message_complex_bit) != 0U)
        return error(darwin::mig::bad_arguments);
    const auto task = state.task_port_pids.find(object);
    if (task == state.task_port_pids.end())
        return error(darwin::mach::invalid_argument);
    const auto process = state.processes.find(task->second);
    if (process == state.processes.end() || process->second.exited)
        return error(darwin::mach::failure);

    using Message = KernelSharedState::MachMessage;
    std::vector<Message::PortTransfer> ports;
    const auto threads = state.task_thread_port_objects.find(task->second);
    if (threads != state.task_thread_port_objects.end()) {
        ports.reserve(threads->second.size());
        for (const auto& [slot, thread] : threads->second) {
            static_cast<void>(slot);
            ports.push_back({ .descriptor_offset = 28U, .sender_name = 0U,
                .array_index = static_cast<std::uint32_t>(ports.size()),
                .object = thread, .right = xnu::ipc::Right::Send,
                .disposition = darwin::mig_wire::disposition_move_send });
        }
    }
    const auto count = static_cast<std::uint32_t>(ports.size());
    const std::array<std::uint32_t, 7> payload { 1U, 0U, count,
        darwin::mig_wire::ool_ports_descriptor_metadata(
            darwin::mig_wire::disposition_move_send, true),
        0U, 1U, count };
    const std::array<Message::OolPortArray, 1> arrays { {
        { .descriptor_offset = 28U, .count = count, .dead_elements = { } }
    } };
    return mach_ipc::enqueue_kernel_reply_locked(
        state, request, identifier, payload, ports, arrays);
}
} // namespace ilemu::task_mig
