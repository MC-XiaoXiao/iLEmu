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
    return mach_ipc::try_task_rpc_locked(memory, state, process,
        registers, bits, reply_name, 24U, 60U,
        [&](std::uint32_t object, KernelSharedState::MachMessage& request) {
            return dispatch_locked(state, object, request);
        });
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
