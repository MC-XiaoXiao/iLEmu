// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "../transport/kernel_reply.hpp"
#include "mach/mach_port_mig_ids.hpp"
#include <limits>

namespace ilemu::port_mig {
class Queries {
public:
    static bool handles(std::uint32_t id)
    {
        using namespace xnu::mig::mach_port;
        return id == xnu::mig::mach_port::id(Routine::mach_port_names) ||
               id == xnu::mig::mach_port::id(Routine::mach_port_type) ||
               id == xnu::mig::mach_port::id(Routine::mach_port_get_refs);
    }

    static std::optional<std::uint32_t> dispatch_locked(
        KernelSharedState& state, std::uint32_t object,
        KernelSharedState::MachMessage& request)
    {
        using namespace mach_support;
        using namespace xnu::mig::mach_port;
        const auto identifier = read_little_word(request.bytes, 20U);
        auto result = evaluate_locked(state, object, request.bytes);
        if (identifier != id(Routine::mach_port_names) || result.error != 0U) {
            const std::array<std::uint32_t, 4> payload { 0U, 1U, result.error,
                result.value };
            return mach_ipc::enqueue_kernel_reply_locked(state, request,
                identifier,
                std::span { payload }.first(result.error == 0U ? 4U : 3U));
        }
        auto reply = make_names_reply(result);
        return mach_ipc::enqueue_kernel_reply_locked(state, request, identifier,
            reply.payload, { }, { }, true, std::move(reply.memory));
    }

    static std::optional<std::uint32_t> try_synchronous_locked(
        AddressSpace& memory, KernelSharedState& state,
        const ProcessContext& process, std::span<const std::uint32_t> registers,
        std::uint32_t bits, std::uint32_t reply_name, std::uint32_t identifier)
    {
        using namespace xnu::mig::mach_port;
        const auto capacity =
            identifier == id(Routine::mach_port_names) ? 76U : 48U;
        if (!memory.accessible(registers[0], capacity, MemoryPermission::Write))
            return std::nullopt;
        std::optional<std::uint32_t> status;
        mach_ipc::try_task_rpc_locked(memory, state, process, registers, bits,
            reply_name, request_size(identifier), capacity,
            [&](std::uint32_t object, KernelSharedState::MachMessage& message) {
                const auto result =
                    evaluate_locked(state, object, message.bytes);
                if (identifier == id(Routine::mach_port_names) &&
                    result.error == 0U) {
                    auto reply = make_names_reply(result);
                    status = mach_ipc::copyout_kernel_reply_locked(memory,
                        state, registers[0], reply_name, *message.reply_object,
                        identifier, reply.payload, true, reply.memory);
                    return status;
                }
                const std::array<std::uint32_t, 4> payload { 0U, 1U,
                    result.error, result.value };
                status = mach_ipc::copyout_kernel_reply_locked(memory, state,
                    registers[0], reply_name, *message.reply_object, identifier,
                    std::span { payload }.first(result.error == 0U ? 4U : 3U));
                return status;
            });
        return status;
    }

private:
    struct Result {
        std::uint32_t error { };
        std::uint32_t value { };
        std::vector<xnu::ipc::NamedEntry> names;
    };

    struct NamesReply {
        std::array<std::uint32_t, 11> payload;
        std::vector<KernelSharedState::MachMessage::OolPayload> memory;
    };

    static NamesReply make_names_reply(const Result& result)
    {
        const auto count = static_cast<std::uint32_t>(result.names.size());
        const auto bytes = count * 4U;
        // Two independent VM copies, not port-right descriptors. A name in
        // these arrays remains meaningful only in the inspected ipc_space.
        std::vector<KernelSharedState::MachMessage::OolPayload> data(2);
        data[0].descriptor_offset = 28U;
        data[1].descriptor_offset = 40U;
        for (auto& buffer : data)
            buffer.bytes.resize(bytes);
        for (std::size_t i = 0; i < result.names.size(); ++i) {
            const auto& entry = result.names[i];
            mach_support::write_little_word(data[0].bytes, i * 4U, entry.name);
            mach_support::write_little_word(data[1].bytes, i * 4U, entry.entry.type);
        }
        // ipc_kmsg_copyout exposes virtual copies with deallocate=TRUE.
        constexpr auto virtual_copy = 0x01000101U;
        const std::array<std::uint32_t, 11> payload { 2U, 0U, bytes,
            virtual_copy, 0U, bytes, virtual_copy, 0U, 1U, count, count };
        return { payload, std::move(data) };
    }

    static std::uint32_t request_size(std::uint32_t identifier)
    {
        using namespace xnu::mig::mach_port;
        if (identifier == id(Routine::mach_port_names))
            return 24U;
        return identifier == id(Routine::mach_port_type) ? 36U : 40U;
    }

    static std::uint32_t notification_type(const KernelSharedState& state,
        std::uint32_t task, std::uint32_t name, bool enumerate)
    {
        const auto request =
            state.mach_dead_name_notifications.find({ task, name });
        if (request == state.mach_dead_name_notifications.end())
            return 0U;
        std::uint32_t type = 0x80000000U; // MACH_PORT_TYPE_DNREQUEST
        // Native mach_port_type masks SPREQUEST bits for old CF callers;
        // mach_port_names exposes the complete request state since XNU1699.
        if (enumerate &&
            state.darwin_abi.mach_port_names ==
                DarwinMachPortNamesAbi::SendPossibleRequests &&
            request->second.send_possible) {
            type |= 0x40000000U;
            if (!request->second.armed)
                type |= 0x20000000U;
        }
        return type;
    }

    static Result evaluate_locked(KernelSharedState& state,
        std::uint32_t object, std::span<const std::byte> bytes)
    {
        using namespace mach_support;
        using namespace xnu::mig::mach_port;
        const auto identifier = read_little_word(bytes, 20U);
        if (bytes.size() != request_size(identifier) ||
            (read_little_word(bytes, 0U) &
                darwin::mig_wire::message_complex_bit))
            return { darwin::mig::bad_arguments, 0U, { } };
        const auto target = state.task_port_pids.find(object);
        if (target == state.task_port_pids.end())
            return { darwin::mach::invalid_task, 0U, { } };
        const auto task = target->second;
        const auto process = state.processes.find(task);
        if (process == state.processes.end() || process->second.exited ||
            !state.mach_namespaces.contains_task(task))
            return { darwin::mach::invalid_task, 0U, { } };
        if (identifier == id(Routine::mach_port_names)) {
            auto entries = state.mach_namespaces.entries(task);
            if (entries.size() > std::numeric_limits<std::uint32_t>::max() / 4U)
                return { darwin::mach::resource_shortage, 0U, { } };
            for (auto& entry : entries)
                entry.entry.type |=
                    notification_type(state, task, entry.name, true);
            return { 0U, 0U, std::move(entries) };
        }
        const auto name = read_little_word(bytes, 32U);
        if (identifier == id(Routine::mach_port_type)) {
            const auto type = state.mach_namespaces.type(task, name);
            return type ? Result { 0U,
                *type | notification_type(state, task, name, false), { } }
                        : Result { darwin::mach::invalid_name, 0U, { } };
        }
        const auto right = read_little_word(bytes, 36U);
        if (right > static_cast<std::uint32_t>(xnu::ipc::Right::DeadName))
            return { 18U, 0U, { } }; // KERN_INVALID_VALUE
        if (name == xnu::ipc::null_name || name == xnu::ipc::dead_name) {
            if (right == static_cast<std::uint32_t>(xnu::ipc::Right::Send) ||
                right == static_cast<std::uint32_t>(xnu::ipc::Right::SendOnce))
                return { 0U, 1U, { } };
            return { darwin::mach::invalid_name, 0U, { } };
        }
        if (!state.mach_namespaces.contains(task, name))
            return { darwin::mach::invalid_name, 0U, { } };
        const auto count = state.mach_namespaces.user_references(
            task, name, static_cast<xnu::ipc::Right>(right));
        return { 0U, count.value_or(0U), { } };
    }
};
} // namespace ilemu::port_mig
