// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "../support.hpp"
#include "foundation/address_space.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "mach/mig_wire_abi.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <optional>
#include <span>

namespace ilemu::mach_ipc {
// Direct handoff of an uncontended COPY_SEND/MAKE_SEND_ONCE simple RPC.
// The caller validates the rights, empty receive queue and default trailer.
inline std::uint32_t copyout_simple_kernel_reply_locked(AddressSpace& memory,
    KernelSharedState& state, std::uint32_t address, std::uint32_t receive_name,
    std::uint32_t receive_object, std::uint32_t identifier,
    std::span<const std::uint32_t> payload)
{
    assert(payload.size() <= 5U);
    std::array<std::uint32_t, 13> words { };
    const auto size = 24U + static_cast<std::uint32_t>(payload.size_bytes());
    words[0] = 18U << 8U;
    words[1] = size;
    words[3] = receive_name;
    words[5] = identifier + 100U;
    std::copy(payload.begin(), payload.end(), words.begin() + 6);
    words[size / 4U + 1U] = darwin::mig_wire::trailer_minimum_size;
    static_cast<void>(
        state.mach_port_objects.increment_sequence_number(receive_object));
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
    std::uint32_t identifier, std::span<const std::uint32_t> payload)
{
    using namespace mach_support;
    const auto destination = request.reply_object;
    const auto right = request.reply_right;
    if (!destination || !right ||
        (*right != xnu::ipc::Right::Send &&
            *right != xnu::ipc::Right::SendOnce) ||
        !state.mach_port_objects.contains(*destination)) {
        discard_mach_message_rights_locked(state, request);
        return std::nullopt;
    }
    KernelSharedState::MachMessage reply;
    reply.bytes = std::move(request.bytes);
    reply.bytes.resize(
        darwin::mig_wire::message_header_size + payload.size_bytes());
    const auto disposition = *right == xnu::ipc::Right::Send ? 17U : 18U;
    const std::uint32_t header[] { disposition,
        static_cast<std::uint32_t>(reply.bytes.size()), *destination, 0, 0,
        identifier + 100U };
    for (std::size_t i = 0; i < 6; ++i)
        write_little_word(reply.bytes, i * 4U, header[i]);
    for (std::size_t i = 0; i < payload.size(); ++i)
        write_little_word(reply.bytes, 24U + i * 4U, payload[i]);
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
