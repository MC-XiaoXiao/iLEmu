// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "../support.hpp"
#include "foundation/address_space.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "mach/mig_wire_abi.hpp"
#include "memory_copyout.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ilemu::mach_ipc {
// Validate an uncontended fixed task RPC before dispatching in-kernel.
// The service either queues its reply or uses the shared direct copyout.
template <typename Dispatch>
bool try_task_rpc_locked(AddressSpace& memory, KernelSharedState& state,
    const ProcessContext& process, std::span<const std::uint32_t> registers,
    std::uint32_t bits, std::uint32_t reply_name, std::uint32_t request_size,
    std::uint32_t minimum_reply_size, Dispatch&& dispatch)
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
        (registers[1] & ~options) != 0U || registers[2] != request_size ||
        registers[3] < minimum_reply_size || registers[4] != reply_name)
        return false;
    const auto target = memory.read32(registers[0] + 8U);
    const auto object = target ? resolve_name_with_right(state, process.pid,
                                     *target, xnu::ipc::Right::Send)
                               : std::nullopt;
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
    request.bytes.resize(request_size);
    if (!memory.copy_out(registers[0], request.bytes))
        return false;
    // COPY_SEND does not consume a task uref; MAKE_SEND_ONCE is created
    // directly in transit under mach_mutex, as in ipc_kobject_server.
    request.reply_object = reply;
    request.reply_right = xnu::ipc::Right::SendOnce;
    return dispatch(*object, request).has_value();
}

// Direct handoff of an uncontended COPY_SEND/MAKE_SEND_ONCE RPC.
// The caller validates the rights, empty receive queue and default trailer.
inline std::uint32_t copyout_kernel_reply_locked(AddressSpace& memory,
    KernelSharedState& state, std::uint32_t address, std::uint32_t receive_name,
    std::uint32_t receive_object, std::uint32_t identifier,
    std::span<const std::uint32_t> payload, bool complex = false,
    std::span<const KernelSharedState::MachMessage::OolPayload> buffers = { })
{
    assert(payload.size() <= 11U);
    std::array<std::uint32_t, 19> words { };
    const auto size = 24U + static_cast<std::uint32_t>(payload.size_bytes());
    words[0] =
        (18U << 8U) | (complex ? darwin::mig_wire::message_complex_bit : 0U);
    words[1] = size;
    words[3] = receive_name;
    words[5] = identifier + 100U;
    std::copy(payload.begin(), payload.end(), words.begin() + 6);
    words[size / 4U + 1U] = darwin::mig_wire::trailer_minimum_size;
    static_cast<void>(
        state.mach_port_objects.increment_sequence_number(receive_object));
    for (const auto& buffer : buffers) {
        assert(buffer.descriptor_offset % 4U == 0U &&
               buffer.descriptor_offset + 4U <= size);
        const auto copied = copyout_memory(memory, buffer.bytes);
        if (!copied)
            return darwin::mach_message::receive_invalid_data;
        words[buffer.descriptor_offset / 4U] = *copied;
    }
    if (!memory.accessible(address, size + 8U, MemoryPermission::Write) ||
        !memory.copy_in(
            address, std::as_bytes(std::span { words }).first(size + 8U)))
        return darwin::mach_message::receive_invalid_data;
    return darwin::mach::success;
}

// ipc_kobject_server transfers the request reply right into a new message.
// The caller holds mach_mutex; ordinary receive handles copyout and trailers.
inline std::optional<std::uint32_t> enqueue_kernel_reply_locked(
    KernelSharedState& state, KernelSharedState::MachMessage& request,
    std::uint32_t identifier, std::span<const std::uint32_t> payload,
    std::span<const KernelSharedState::MachMessage::PortTransfer> ports = { },
    std::span<const KernelSharedState::MachMessage::OolPortArray> arrays = { },
    bool complex = false,
    std::vector<KernelSharedState::MachMessage::OolPayload> memory = { })
{
    using namespace mach_support;
    const auto destination = request.reply_object;
    const auto right = request.reply_right;
    if (!destination || !right ||
        (*right != xnu::ipc::Right::Send &&
            *right != xnu::ipc::Right::SendOnce) ||
        !state.mach_port_objects.contains(*destination)) {
        // A produced send-once token must be destroyed even when the caller
        // supplied no usable reply port (ipc_kobject_server destroys reply).
        for (const auto& port : ports) {
            if (port.right == xnu::ipc::Right::SendOnce)
                enqueue_send_once_notification_locked(state, port.object);
        }
        discard_mach_message_rights_locked(state, request);
        return std::nullopt;
    }
    KernelSharedState::MachMessage reply;
    reply.bytes = std::move(request.bytes);
    reply.bytes.resize(
        darwin::mig_wire::message_header_size + payload.size_bytes());
    const auto disposition = *right == xnu::ipc::Right::Send ? 17U : 18U;
    const std::uint32_t header[] {
        disposition |
            (!complex && ports.empty() && arrays.empty() && memory.empty()
                    ? 0U
                    : darwin::mig_wire::message_complex_bit),
        static_cast<std::uint32_t>(reply.bytes.size()), *destination, 0, 0,
        identifier + 100U
    };
    for (std::size_t i = 0; i < 6; ++i)
        write_little_word(reply.bytes, i * 4U, header[i]);
    for (std::size_t i = 0; i < payload.size(); ++i)
        write_little_word(reply.bytes, 24U + i * 4U, payload[i]);
    reply.port_transfers.assign(ports.begin(), ports.end());
    reply.ool_port_arrays.assign(arrays.begin(), arrays.end());
    reply.ool_payloads = std::move(memory);
    for (const auto& port : ports) {
        // Newly produced Send rights become message-held only after a live
        // reply destination exists; receiver copyout installs the user refs.
        assert(port.right == xnu::ipc::Right::Send ||
               port.right == xnu::ipc::Right::SendOnce);
        if (port.right == xnu::ipc::Right::Send)
            ++state.mach_inflight_send_rights[port.object];
    }
    reply.destination = *destination;
    if (*right == xnu::ipc::Right::Send)
        reply.destination_send_object = destination;
    request.reply_object.reset();
    request.reply_right.reset();
    discard_mach_message_rights_locked(state, request);
    state.enqueue_mach_message_locked(*destination, std::move(reply));
    return destination;
}
} // namespace ilemu::mach_ipc
