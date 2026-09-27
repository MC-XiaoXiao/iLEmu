// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "../transport/kernel_reply.hpp"
#include "kernel/kernel_mach_task_identity.hpp"
#include "mach/task_mig_ids.hpp"

namespace ilemu::task_mig {
class SpecialPorts {
public:
    static std::optional<std::uint32_t> try_synchronous_locked(
        AddressSpace& memory, KernelSharedState& state,
        const ProcessContext& process, std::span<const std::uint32_t> registers,
        std::uint32_t bits, std::uint32_t reply_name,
        std::uint32_t receive_address)
    {
        // Faulting buffers retain ordinary IPC copyout and cleanup semantics.
        if (!memory.accessible(receive_address, 48U, MemoryPermission::Write))
            return std::nullopt;
        std::optional<std::uint32_t> status;
        mach_ipc::try_task_rpc_locked(memory, state, process, registers, bits,
            reply_name, 36U, 48U,
            [&](std::uint32_t object, KernelSharedState::MachMessage& request)
                -> std::optional<std::uint32_t> {
                const auto which =
                    mach_support::read_little_word(request.bytes, 32U);
                const auto result = query_locked(state, object, which);
                const auto identifier =
                    mach_support::read_little_word(request.bytes, 20U);
                if (result.error != darwin::mach::success) {
                    const std::array<std::uint32_t, 3> payload { 0U, 1U,
                        result.error };
                    status = mach_ipc::copyout_kernel_reply_locked(memory,
                        state, receive_address, reply_name,
                        *request.reply_object, identifier, payload);
                } else {
                    auto name = result.port;
                    if (valid(name)) {
                        const auto copied =
                            state.mach_namespaces.copyout(process.pid, name,
                                xnu::ipc::type_mask(xnu::ipc::Right::Send));
                        if (!copied)
                            return std::nullopt;
                        name = *copied;
                        if (which == 3U)
                            static_cast<void>(state.mach_port_objects
                                    .increment_make_send_count(result.port));
                    }
                    const std::array<std::uint32_t, 4> payload { 1U, name, 0U,
                        17U << 16U };
                    status = mach_ipc::copyout_kernel_reply_locked(memory,
                        state, receive_address, reply_name,
                        *request.reply_object, identifier, payload, true);
                }
                return status;
            });
        return status;
    }

    static bool handles(std::uint32_t identifier)
    {
        using namespace xnu::mig::task;
        return identifier == id(Routine::task_get_special_port) ||
               identifier == id(Routine::task_set_special_port);
    }

    static std::optional<std::uint32_t> dispatch_locked(
        KernelSharedState& state, std::uint32_t object,
        KernelSharedState::MachMessage& request)
    {
        using namespace mach_support;
        using namespace xnu::mig::task;
        const auto identifier = read_little_word(request.bytes, 20U);
        const bool set = identifier == id(Routine::task_set_special_port);
        auto error = darwin::mig::bad_arguments;
        std::uint32_t port = 0;
        const bool complex = (read_little_word(request.bytes, 0U) &
                                 darwin::mig_wire::message_complex_bit) != 0U;
        if (request.bytes.size() == (set ? 52U : 36U) && complex == set &&
            (!set || read_little_word(request.bytes, 24U) == 1U)) {
            // Copyin has already consumed moved rights. MIG checks the
            // received send-right type, including NULL and DEAD tokens.
            const auto descriptor =
                set ? read_little_word(request.bytes, 36U) : 0U;
            if (set && ((descriptor >> 24U) != 0U ||
                           darwin::mig_wire::received_port_disposition(
                               (descriptor >> 16U) & 0xffU) != 17U)) {
                error = darwin::mig::type_error;
            } else {
                const auto which =
                    read_little_word(request.bytes, set ? 48U : 32U);
                const auto target = state.task_port_pids.find(object);
                if (target == state.task_port_pids.end() ||
                    !supports(
                        state.darwin_abi.task_special_ports, which, set)) {
                    error = darwin::mach::invalid_argument;
                } else {
                    const auto process = state.processes.find(target->second);
                    if (process == state.processes.end() ||
                        process->second.exited) {
                        error = darwin::mach::failure;
                    } else if (set) {
                        const auto name = read_little_word(request.bytes, 28U);
                        // The descriptor still contains a sender name. Resolve
                        // only through the captured right, never another space.
                        const auto incoming =
                            request.port_transfers.empty()
                                ? name
                                : request.port_transfers.front().object;
                        const auto old =
                            mach_task_identity::special_port_locked(
                                state, object, which);
                        if ((which == 7U || which == 9U) && valid(old)) {
                            error =
                                8U; // KERN_NO_ACCESS: write-once kernel slots
                        } else {
                            if (valid(incoming))
                                retain_kernel_send_right_locked(
                                    state, incoming);
                            state.task_special_ports[object][which] = incoming;
                            if (valid(old))
                                release_kernel_send_right_locked(state, old);
                            error = darwin::mach::success;
                        }
                    } else {
                        const auto result = query_locked(state, object, which);
                        port = result.port;
                        if (which == 3U && valid(port))
                            static_cast<void>(state.mach_port_objects
                                    .increment_make_send_count(port));
                        error = result.error;
                    }
                }
            }
        }
        if (set || error != darwin::mach::success) {
            const std::array<std::uint32_t, 3> payload { 0U, 1U, error };
            return mach_ipc::enqueue_kernel_reply_locked(
                state, request, identifier, payload);
        }
        const std::array<std::uint32_t, 4> payload { 1U, port, 0U, 17U << 16U };
        const std::array<KernelSharedState::MachMessage::PortTransfer, 1>
            ports {
                { { 28U, 0U, std::nullopt, port, xnu::ipc::Right::Send, 17U } }
            };
        const auto transferred =
            std::span { ports }.first(valid(port) ? 1U : 0U);
        return mach_ipc::enqueue_kernel_reply_locked(
            state, request, identifier, payload, transferred, { }, true);
    }

private:
    struct QueryResult {
        std::uint32_t port { };
        std::uint32_t error { };
    };

    static QueryResult query_locked(
        KernelSharedState& state, std::uint32_t object, std::uint32_t which)
    {
        const auto target = state.task_port_pids.find(object);
        if (target == state.task_port_pids.end() ||
            !supports(state.darwin_abi.task_special_ports, which, false))
            return { 0U, darwin::mach::invalid_argument };
        const auto process = state.processes.find(target->second);
        if (process == state.processes.end() || process->second.exited)
            return { 0U, darwin::mach::failure };
        auto port =
            which == 3U
                ? mach_task_identity::name_port_locked(state, target->second)
                : mach_task_identity::special_port_locked(state, object, which);
        if (valid(port) && !state.mach_port_objects.contains(port))
            port = xnu::ipc::dead_name;
        return { port, darwin::mach::success };
    }

    static bool valid(std::uint32_t object)
    {
        return object != xnu::ipc::null_name && object != xnu::ipc::dead_name;
    }

    static bool supports(
        DarwinTaskSpecialPortsAbi abi, std::uint32_t which, bool set)
    {
        using Abi = DarwinTaskSpecialPortsAbi;
        switch (which) {
        case 1U:
        case 2U:
        case 4U:
            return true;
        case 3U:
            return !set && abi != Abi::Ledger;
        case 5U:
        case 6U:
            return abi == Abi::Ledger || abi == Abi::SecurityLedger ||
                   abi == Abi::SecurityLedgerAutomount;
        case 7U:
        case 9U:
            return abi != Abi::Ledger;
        case 8U:
            return abi == Abi::SecurityLedger ||
                   abi == Abi::SecurityLedgerAutomount;
        case 10U:
            return abi == Abi::SecurityLedgerAutomount ||
                   abi == Abi::SecurityDebug;
        default:
            return false;
        }
    }
};
} // namespace ilemu::task_mig
