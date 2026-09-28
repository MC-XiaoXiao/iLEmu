// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "port/notifications.hpp"
#include "transport/kernel_reply.hpp"
#include "transport/port_copyout.hpp"

namespace ilemu::port_mig {
using namespace mach_support;

std::optional<Notifications::SynchronousReply> Notifications::try_synchronous_locked(AddressSpace& memory,
    KernelSharedState& state, const ProcessContext& process,
    std::span<const std::uint32_t> registers, std::uint32_t bits,
    std::uint32_t reply_name, std::uint32_t receive_address)
{
    std::optional<SynchronousReply> reply;
    mach_ipc::try_task_rpc_locked(memory, state, process, registers,
        bits, reply_name, 60U, 48U,
        [&](std::uint32_t object, KernelSharedState::MachMessage& request)
            -> std::optional<std::uint32_t> {
            if (read_little_word(request.bytes, 16U) != 0U ||
                read_little_word(request.bytes, 24U) != 1U)
                return std::nullopt;
            const auto descriptor = read_little_word(request.bytes, 36U);
            const auto name = read_little_word(request.bytes, 28U);
            if (name == xnu::ipc::null_name || name == xnu::ipc::dead_name) {
                if (descriptor != (18U << 16U) && descriptor != (21U << 16U))
                    return std::nullopt;
            } else {
                if (descriptor != (21U << 16U))
                    return std::nullopt;
                const auto notify = resolve_name_with_right(
                    state, process.pid, name, xnu::ipc::Right::Receive);
                if (!notify || !state.mach_port_objects.contains(*notify))
                    return std::nullopt;
                // MAKE_SEND_ONCE creates a token in transit without changing
                // sender urefs. The shared service retains or destroys it.
                state.mach_port_objects.make_send_once(*notify);
                request.port_transfers.push_back({ 28U, name, std::nullopt,
                    *notify, xnu::ipc::Right::SendOnce, 21U });
            }
            const auto result = evaluate_locked(state, object, request);
            const auto receive = *request.reply_object;
            const auto queue = state.mach_queues.find(receive);
            const bool cleanup_to_reply = result.error != 0U &&
                std::any_of(request.port_transfers.begin(),
                    request.port_transfers.end(), [&](const auto& port) {
                        return port.object == receive;
                    });
            // Immediate notification delivery and error cleanup may precede
            // the RPC reply. Only bypass receive when neither can intervene.
            if (!cleanup_to_reply && queue != state.mach_queues.end() &&
                queue->second.empty() &&
                memory.accessible(receive_address, 48U, MemoryPermission::Write)) {
                std::optional<std::uint32_t> previous { result.previous };
                if (result.error == 0U && result.previous != 0U)
                    previous = mach_transport::PortCopyout { state, process.pid }(
                        result.previous, xnu::ipc::Right::SendOnce);
                if (previous) {
                    const auto identifier = read_little_word(request.bytes, 20U);
                    const std::array<std::uint32_t, 4> payload {
                        result.error == 0U ? 1U : 0U,
                        result.error == 0U ? *previous : 1U,
                        result.error, 18U << 16U };
                    request.reply_object.reset();
                    request.reply_right.reset();
                    discard_mach_message_rights_locked(state, request);
                    const auto status = mach_ipc::copyout_kernel_reply_locked(
                        memory, state, receive_address, reply_name, receive,
                        identifier,
                        std::span { payload }.first(result.error == 0U ? 4U : 3U),
                        result.error == 0U);
                    reply = SynchronousReply { status };
                    return status;
                }
            }
            const auto destination = reply_locked(state, request, result);
            if (destination)
                reply = SynchronousReply { };
            return destination;
        }, true);
    return reply;
}

std::optional<std::uint32_t> Notifications::dispatch_locked(
    KernelSharedState& state, std::uint32_t object,
    KernelSharedState::MachMessage& request)
{
    return reply_locked(state, request, evaluate_locked(state, object, request));
}

Notifications::Result Notifications::evaluate_locked(KernelSharedState& state,
    std::uint32_t object, KernelSharedState::MachMessage& request)
{
    Result result { darwin::mig::bad_arguments, 0U };
    if (request.bytes.size() == 60U &&
        (read_little_word(request.bytes, 0U) &
            darwin::mig_wire::message_complex_bit) != 0U &&
        read_little_word(request.bytes, 24U) == 1U) {
        const auto descriptor = read_little_word(request.bytes, 36U);
        if ((descriptor >> 24U) != 0U ||
            darwin::mig_wire::received_port_disposition(
                (descriptor >> 16U) & 0xffU) != 18U) {
            result.error = darwin::mig::type_error;
        } else {
            auto notify = read_little_word(request.bytes, 28U);
            for (const auto& transfer : request.port_transfers) {
                if (transfer.descriptor_offset == 28U)
                    notify = transfer.object;
            }
            result = register_locked(state, object,
                read_little_word(request.bytes, 48U),
                read_little_word(request.bytes, 52U),
                read_little_word(request.bytes, 56U), notify);
            if (result.error == 0U) {
                // The request now owns (or has delivered) this send-once
                // token. Failed calls leave it for ordinary IPC destruction.
                std::erase_if(request.port_transfers, [](const auto& port) {
                    return port.descriptor_offset == 28U;
                });
            }
        }
    }
    return result;
}

std::optional<std::uint32_t> Notifications::reply_locked(KernelSharedState& state,
    KernelSharedState::MachMessage& request, Result result)
{
    const auto identifier = read_little_word(request.bytes, 20U);
    if (result.error != 0U) {
        const std::array<std::uint32_t, 3> payload { 0U, 1U, result.error };
        return mach_ipc::enqueue_kernel_reply_locked(
            state, request, identifier, payload);
    }
    auto previous = result.previous;
    if (previous != 0U && !state.mach_port_objects.contains(previous))
        previous = xnu::ipc::dead_name;
    const std::array<std::uint32_t, 4> payload {
        1U, previous, 0U, 18U << 16U };
    const std::array<KernelSharedState::MachMessage::PortTransfer, 1> ports {
        { { 28U, 0U, std::nullopt, previous, xnu::ipc::Right::SendOnce, 18U } }
    };
    return mach_ipc::enqueue_kernel_reply_locked(state, request, identifier,
        payload, std::span { ports }.first(
            previous != 0U && previous != xnu::ipc::dead_name ? 1U : 0U),
        { }, true);
}
Notifications::Result Notifications::register_locked(KernelSharedState& state,
    std::uint32_t object, std::uint32_t name, std::uint32_t kind,
    std::uint32_t sync, std::uint32_t notify)
{
    // mach_port_request_notification: validation precedes request replacement.
    const auto target = state.task_port_pids.find(object);
    if (target == state.task_port_pids.end())
        return { darwin::mach::invalid_task };
    const auto task = target->second;
    const auto process = state.processes.find(task);
    if (process == state.processes.end() || process->second.exited ||
        !state.mach_namespaces.contains_task(task))
        return { darwin::mach::invalid_task };
    if (notify == xnu::ipc::dead_name)
        return { 20U }; // KERN_INVALID_CAPABILITY
    const bool modern = state.darwin_abi.mach_port_requests ==
                        DarwinMachPortRequestAbi::SendPossibleRequests;
    const bool send_possible = kind == mach_notify_send_possible;
    const bool name_request = kind == mach_notify_dead_name || send_possible;
    if ((!name_request && kind != mach_notify_port_destroyed &&
            kind != mach_notify_no_senders) || (send_possible && !modern))
        return { 18U }; // KERN_INVALID_VALUE
    if (kind == mach_notify_port_destroyed && sync != 0U)
        return { 18U };
    if (name == xnu::ipc::null_name || name == xnu::ipc::dead_name)
        return { name_request ? darwin::mach::invalid_argument :
                                darwin::mach::invalid_right };
    const auto entry = state.mach_namespaces.lookup(task, name);
    if (!entry)
        return { darwin::mach::invalid_name };
    const auto receive = xnu::ipc::type_mask(xnu::ipc::Right::Receive);
    if (!name_request) {
        if ((entry->type & receive) == 0U)
            return { darwin::mach::invalid_right };
        const auto key = std::pair { entry->object, kind };
        Result result;
        if (const auto old = state.mach_notifications.find(key);
            old != state.mach_notifications.end()) {
            result.previous = old->second.notify_object;
            state.mach_notifications.erase(old);
        }
        if (notify != 0U) {
            state.mach_notifications.emplace(key,
                KernelSharedState::MachNotificationRequest { notify, sync });
            if (kind == mach_notify_no_senders)
                static_cast<void>(enqueue_no_senders_notification_locked(
                    state, entry->object));
        }
        return result;
    }
    const auto key = std::pair { task, name };
    const auto old = state.mach_dead_name_notifications.find(key);
    // XNU1699 checks empty cancellation before the entry type; XNU792 does not.
    if (modern && notify == 0U && old == state.mach_dead_name_notifications.end())
        return { };
    const auto port_rights = receive |
        xnu::ipc::type_mask(xnu::ipc::Right::Send) |
        xnu::ipc::type_mask(xnu::ipc::Right::SendOnce);
    if ((entry->type & port_rights) == 0U) {
        if ((entry->type & xnu::ipc::type_mask(xnu::ipc::Right::DeadName)) == 0U)
            return { darwin::mach::invalid_right };
        if ((!send_possible && sync == 0U) || notify == 0U)
            return { darwin::mach::invalid_argument };
        // The supported XNU792–2782 contracts reject an overflowing uref.
        if (!state.mach_namespaces.modify_references(
                task, name, xnu::ipc::Right::DeadName, 1))
            return { 19U }; // KERN_UREFS_OVERFLOW
        enqueue_dead_name_notification_locked(state, notify, name);
        return { };
    }
    Result result;
    if (old != state.mach_dead_name_notifications.end()) {
        result.previous = old->second.notify_object;
        state.mach_dead_name_notifications.erase(old);
    }
    if (notify != 0U) {
        state.mach_dead_name_notifications.emplace(key,
            KernelSharedState::MachDeadNameNotificationRequest {
                entry->object, notify, sync, send_possible, sync != 0U });
        if (send_possible && sync != 0U) {
            state.mach_send_possible_armed_destinations.insert(entry->object);
            state.notify_send_possible_locked(entry->object);
        }
    }
    return result;
}
} // namespace ilemu::port_mig
