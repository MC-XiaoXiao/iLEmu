// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "../support.hpp"
#include "../transport/kernel_reply.hpp"
#include "guarded.hpp"
#include "mach/mach_port_mig_ids.hpp"
#include <algorithm>
#include <bit>
namespace ilemu::port_mig {
class Guarded {
    using Routine = xnu::mig::mach_port::Routine;
    static constexpr auto construct =
        xnu::mig::mach_port::id(Routine::mach_port_construct);
    static constexpr auto guard =
        xnu::mig::mach_port::id(Routine::mach_port_guard);
    static constexpr auto unguard =
        xnu::mig::mach_port::id(Routine::mach_port_unguard);

public:
    static bool handles(std::uint32_t id)
    {
        return id >= construct && id <= unguard;
    }
    static std::optional<std::uint32_t> dispatch_locked(
        KernelSharedState& state, std::uint32_t object,
        KernelSharedState::MachMessage& request)
    {
        const auto result = evaluate_locked(state, object, request);
        const auto id = mach_support::read_little_word(request.bytes, 20U);
        const std::array<std::uint32_t, 4> words { 0U, 1U, result.error,
            result.name };
        return mach_ipc::enqueue_kernel_reply_locked(state, request, id,
            std::span { words }.first(
                result.error == 0U && id == construct ? 4U : 3U));
    }

private:
    static GuardedPorts::Result evaluate_locked(KernelSharedState& state,
        std::uint32_t object, const KernelSharedState::MachMessage& request)
    {
        using namespace mach_support;
        // Guard operations and their status bits share the guarded IPC
        // contract.
        if (state.darwin_abi.mach_port_status !=
            DarwinMachPortStatusAbi::ImportanceAndGuards)
            return { darwin::mig::bad_id };
        const auto bytes = std::span { request.bytes };
        const auto id = read_little_word(bytes, 20U);
        const bool constructing = id == construct;
        const auto size = constructing ? 56U : id == unguard ? 44U : 48U;
        const bool complex = (read_little_word(bytes, 0U) &
                                 darwin::mig_wire::message_complex_bit) != 0U;
        if (bytes.size() != size || complex != constructing ||
            (constructing && read_little_word(bytes, 24U) != 1U))
            return { darwin::mig::bad_arguments };
        const KernelSharedState::MachMessage::OolPayload* options = nullptr;
        if (constructing) {
            // Native MIG checks both descriptor type and fixed pointee size.
            if ((read_little_word(bytes, 36U) >> 24U) != 1U ||
                read_little_word(bytes, 32U) != GuardedPorts::options_size)
                return { darwin::mig::type_error };
            const auto payload = std::ranges::find(request.ool_payloads, 28U,
                &KernelSharedState::MachMessage::OolPayload::descriptor_offset);
            if (payload == request.ool_payloads.end() ||
                payload->bytes.size() != GuardedPorts::options_size)
                return { darwin::mig::type_error };
            options = &*payload;
        }
        const auto target = state.task_port_pids.find(object);
        if (target == state.task_port_pids.end())
            return { darwin::mach::invalid_task };
        const auto process = state.processes.find(target->second);
        if (process == state.processes.end() || process->second.exited ||
            !state.mach_namespaces.contains_task(target->second))
            return { darwin::mach::invalid_task };
        const auto wide = [&](std::size_t offset) {
            return static_cast<std::uint64_t>(read_little_word(bytes, offset)) |
                   (static_cast<std::uint64_t>(
                        read_little_word(bytes, offset + 4U))
                       << 32U);
        };
        if (constructing)
            return GuardedPorts::construct_locked(state, target->second,
                { read_little_word(options->bytes, 0U),
                    read_little_word(options->bytes, 4U) },
                wide(48U));
        const auto operation = id == guard ? GuardedPorts::Operation::Guard
                               : id == unguard
                                   ? GuardedPorts::Operation::Unguard
                                   : GuardedPorts::Operation::Destruct;
        const bool destroying = operation == GuardedPorts::Operation::Destruct;
        return { GuardedPorts::change_locked(state, target->second,
            read_little_word(bytes, 32U), operation,
            wide(destroying ? 40U : 36U),
            destroying
                ? std::bit_cast<std::int32_t>(read_little_word(bytes, 36U))
                : 0,
            id == guard && read_little_word(bytes, 44U) != 0U) };
    }
};
} // namespace ilemu::port_mig
