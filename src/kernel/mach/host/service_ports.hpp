// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "../transport/kernel_reply.hpp"
#include "kernel/kernel_mach_task_identity.hpp"
#include "mach/mach_host_mig_ids.hpp"

namespace ilemu::host_mig {
class ServicePorts {
public:
    static bool handles(std::uint32_t identifier)
    {
        using namespace xnu::mig::mach_host;
        return identifier == id(Routine::host_get_io_master) ||
               identifier == id(Routine::host_get_clock_service);
    }

    static std::optional<std::uint32_t> dispatch_locked(
        KernelSharedState& state, std::uint32_t host,
        KernelSharedState::MachMessage& request)
    {
        const auto result = evaluate(host, request.bytes);
        const auto identifier =
            mach_support::read_little_word(request.bytes, 20U);
        if (result.error != 0U) {
            const std::array<std::uint32_t, 3> payload { 0U, 1U, result.error };
            return mach_ipc::enqueue_kernel_reply_locked(
                state, request, identifier, payload);
        }
        const auto payload = port_payload(result.object);
        const std::array<KernelSharedState::MachMessage::PortTransfer, 1>
            ports { KernelSharedState::MachMessage::PortTransfer { 28U,
                result.object, std::nullopt, result.object,
                xnu::ipc::Right::Send, 17U } };
        return mach_ipc::enqueue_kernel_reply_locked(
            state, request, identifier, payload, ports);
    }

    static std::optional<std::uint32_t> try_synchronous_locked(
        AddressSpace& memory, KernelSharedState& state,
        const ProcessContext& process, std::span<const std::uint32_t> registers,
        std::uint32_t bits, std::uint32_t reply_name,
        std::uint32_t receive_address)
    {
        using namespace mach_support;
        constexpr auto send_receive = darwin::mach_message::option_send |
                                      darwin::mach_message::option_receive;
        constexpr auto rpc_options =
            send_receive | darwin::mach_message::option_receive_timeout |
            darwin::mach_message::option_receive_large;
        if (bits != (darwin::mig_wire::disposition_copy_send |
                        (darwin::mig_wire::disposition_make_send_once << 8U)) ||
            (registers[1] & send_receive) != send_receive ||
            (registers[1] & ~rpc_options) != 0U ||
            (registers[2] != 24U && registers[2] != 36U) ||
            registers[3] < 48U || registers[4] != reply_name)
            return std::nullopt;
        const auto reply = resolve_name_with_right(
            state, process.pid, reply_name, xnu::ipc::Right::Receive);
        if (!reply || !state.mach_port_objects.contains(*reply) ||
            state.mach_port_set_links_by_member.contains(*reply))
            return std::nullopt;
        const auto queue = state.mach_queues.find(*reply);
        if (queue == state.mach_queues.end() || !queue->second.empty())
            return std::nullopt;
        std::array<std::byte, 36> request;
        const auto bytes = std::span { request }.first(registers[2]);
        if (!memory.copy_out(registers[0], bytes))
            return darwin::mach_message::send_invalid_data;
        // Faulting copyout needs generic IPC's header/body rights cleanup.
        if (!memory.accessible(receive_address, 48U, MemoryPermission::Write))
            return std::nullopt;
        const auto result =
            evaluate(mach_task_identity::initial_host_self_name, bytes);
        const auto identifier = read_little_word(bytes, 20U);
        if (result.error != 0U) {
            const std::array<std::uint32_t, 3> payload { 0U, 1U, result.error };
            return mach_ipc::copyout_kernel_reply_locked(memory, state,
                receive_address, reply_name, *reply, identifier, payload);
        }
        const auto name = state.mach_namespaces.copyout(process.pid,
            result.object, xnu::ipc::type_mask(xnu::ipc::Right::Send));
        if (!name)
            return std::nullopt;
        const auto payload = port_payload(*name);
        return mach_ipc::copyout_kernel_reply_locked(memory, state,
            receive_address, reply_name, *reply, identifier, payload, true);
    }

private:
    struct Result {
        std::uint32_t object { };
        std::uint32_t error { };
    };

    static Result evaluate(std::uint32_t host, std::span<const std::byte> bytes)
    {
        using namespace mach_support;
        using namespace mach_task_identity;
        using namespace xnu::mig::mach_host;
        const auto io =
            read_little_word(bytes, 20U) == id(Routine::host_get_io_master);
        if (bytes.size() != (io ? 24U : 36U) ||
            (read_little_word(bytes, 0U) &
                darwin::mig_wire::message_complex_bit))
            return { 0U, darwin::mig::bad_arguments };
        // Native MIG selects the routine before convert_port_to_host; a valid
        // non-host kernel object therefore produces KERN_INVALID_ARGUMENT.
        if (host != initial_host_self_name)
            return { 0U, darwin::mach::invalid_argument };
        if (io)
            return { initial_io_master_name, 0U };
        switch (read_little_word(bytes, 32U)) {
        case 0U:
            return { initial_clock_name, 0U };
        case 1U:
            return { initial_calendar_clock_name, 0U };
        default:
            return { 0U, darwin::mach::invalid_argument };
        }
    }

    static std::array<std::uint32_t, 4> port_payload(std::uint32_t name)
    {
        return { 1U, name, 0U, 17U << 16U };
    }
};
} // namespace ilemu::host_mig
