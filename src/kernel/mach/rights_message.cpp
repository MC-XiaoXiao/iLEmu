// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Dispatch guest Mach right insertion, extraction and release
// operations.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/mach/mach_port.defs

#include "port/rights.hpp"
#include "transport/port_copyin.hpp"
#include "transport/port_copyout.hpp"
#include "transport/kernel_reply.hpp"
#include "mach/bootstrap_mig_ids.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/darwin_kqueue_abi.hpp"
#include "network/darwin_network_abi.hpp"
#include "kernel/darwin_resource_abi.hpp"
#include "network/darwin_route_socket.hpp"
#include "kernel/kernel.hpp"
#include "kernel/kernel_clock.hpp"
#include "kernel/kernel_iokit.hpp"
#include "kernel/kernel_mach_ipc.hpp"
#include "kernel/kernel_network.hpp"
#include "kernel/mach_clock_abi.hpp"
#include "mach/mach_host_mig_ids.hpp"
#include "mach/mach_port_mig_ids.hpp"
#include "kernel/mach_scheduler_abi.hpp"
#include "kernel/mach_thread_policy_abi.hpp"
#include "mach/mig_wire_abi.hpp"
#include "mach/task_mig_ids.hpp"
#include "mach/thread_act_mig_ids.hpp"
#include "mach/vm_map_mig_ids.hpp"
#include "mach/xnu_mig_adapter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "support.hpp"

namespace ilemu {

using namespace mach_support;

bool CompatibilityKernel::dispatch_mach_rights_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    auto& registers = cpu.registers();
    const auto message_address = request.address;
    const std::optional<std::uint32_t> bits { request.bits };
    const std::optional<std::uint32_t> remote_port { request.remote_port };
    const std::optional<std::uint32_t> local_port { request.local_port };
    const std::optional<std::uint32_t> message_id { request.identifier };
    if (*message_id ==
            mig_message_id(xnu::mig::task::Routine::semaphore_destroy) &&
        registers[3] >= 36) {
        // semaphore_destroy
        std::uint32_t kernel_result = 0;
        if (*message_id ==
                   mig_message_id(
                       xnu::mig::task::Routine::semaphore_destroy)) {
            constexpr std::uint32_t semaphore_destroy_request_size =
                darwin::mig_wire::complex_descriptor_base +
                darwin::mig_wire::descriptor_size;
            const auto descriptor_count = memory_.read32(
                message_address +
                darwin::mig_wire::complex_descriptor_count_offset);
            const auto semaphore_name =
                memory_.read32(message_address +
                               xnu::mig::task::semaphore_destroy_arguments[1]
                                   .request_offset);
            const auto descriptor_word =
                memory_.read32(message_address +
                               darwin::mig_wire::descriptor_metadata_offset(0));
            const auto disposition =
                descriptor_word
                    ? (*descriptor_word >>
                          darwin::mig_wire::descriptor_disposition_shift) &
                          0xffU
                    : 0U;
            const auto descriptor_type =
                descriptor_word ? *descriptor_word >>
                                      darwin::mig_wire::descriptor_type_shift
                                : std::numeric_limits<std::uint32_t>::max();
            const auto valid_wire =
                registers[2] == semaphore_destroy_request_size &&
                (*bits & darwin::mig_wire::message_complex_bit) != 0 &&
                descriptor_count == 1 && semaphore_name && descriptor_word &&
                disposition == darwin::mig_wire::disposition_move_send &&
                descriptor_type == darwin::mig_wire::port_descriptor_type;
            if (!valid_wire) {
                kernel_result = darwin::mach::invalid_argument;
            } else {
                std::lock_guard mach_lock { shared_state_->mach_mutex };
                const auto target = target_task_for_port(
                    *shared_state_, process_.pid, *remote_port);
                const auto semaphore_object =
                    resolve_name_with_right(*shared_state_, process_.pid,
                        *semaphore_name, xnu::ipc::Right::Send);
                const auto semaphore =
                    semaphore_object
                        ? shared_state_->mach_semaphores.find(*semaphore_object)
                        : shared_state_->mach_semaphores.end();

                // semaphore_consume_ref_t is a MOVE_SEND descriptor. Its send
                // right is consumed by message transport even when the kernel
                // operation fails; this direct kernel-server path performs that
                // transport step here.
                if (semaphore_object) {
                    static_cast<void>(
                        consume_moved_right_locked(*shared_state_, process_.pid,
                            *semaphore_name, xnu::ipc::Right::Send, false));
                }
                if (!target ||
                    semaphore == shared_state_->mach_semaphores.end() ||
                    semaphore->second.owner_pid != *target) {
                    kernel_result = darwin::mach::invalid_argument;
                } else {
                    const auto object = *semaphore_object;
                    for (const auto& waiter : semaphore->second.waiters) {
                        shared_state_->semaphore_terminations.insert(waiter);
                    }
                    output_.write(
                        "[semaphore] destroy pid=" +
                        std::to_string(process_.pid) +
                        " port=" + std::to_string(object) + " waiters=" +
                        std::to_string(semaphore->second.waiters.size()) +
                        "\n");
                    terminate_receive_object_locked(*shared_state_, object);
                }
            }
        }
        const std::array<std::uint32_t, 9> reply {
            18,
            36,
            *local_port,
            0,
            0,
            *message_id + 100,
            0x00000000U,
            0x00000001U,
            kernel_result,
        };
        for (std::size_t index = 0; index < reply.size(); ++index) {
            if (!memory_.write32(
                    message_address + static_cast<std::uint32_t>(index * 4U),
                    reply[index])) {
                registers[0] = 0x10004008U;
                return true;
            }
        }
        registers[0] = 0;
        return true;
    }
    if (*message_id == mig_message_id(xnu::mig::task::Routine::semaphore_create) &&
        registers[3] >= 40) {
        std::uint32_t port = 0;
        bool port_already_copied_out = false;
        if (*message_id ==
            mig_message_id(xnu::mig::task::Routine::semaphore_create)) {
            const auto policy =
                memory_
                    .read32(message_address +
                            xnu::mig::task::semaphore_create_arguments[2]
                                .request_offset)
                    .value_or(8);
            const auto initial_value = static_cast<std::int32_t>(memory_
                    .read32(message_address +
                            xnu::mig::task::semaphore_create_arguments[3]
                                .request_offset)
                    .value_or(0xffffffffU));
            if (policy <= 7 && initial_value >= 0) {
                std::lock_guard mach_lock { shared_state_->mach_mutex };
                // semaphore_create(task, ...) charges the semaphore to the
                // named task, not necessarily to the caller. Keep the owner
                // identity in sync with XNU so task teardown can terminate
                // exactly its objects.
                const auto owner = target_task_for_port(
                    *shared_state_, process_.pid, *remote_port);
                if (owner) {
                    const auto object = shared_state_->allocate_mach_object();
                    shared_state_->mach_semaphores.emplace(
                        object, KernelSharedState::MachSemaphore {
                                    initial_value, *owner, { } });
                    if (shared_state_->mach_port_objects.create(object)) {
                        const auto name =
                            shared_state_->mach_namespaces.copyout(process_.pid,
                                object,
                                xnu::ipc::type_mask(
                                    xnu::ipc::Right::Send));
                        if (name) {
                            port = *name;
                            port_already_copied_out = true;
                            output_.write("[semaphore] create pid=" +
                                          std::to_string(process_.pid) +
                                          " owner=" + std::to_string(*owner) +
                                          " object=" + std::to_string(object) +
                                          " name=" + std::to_string(port) +
                                          " value=" +
                                          std::to_string(initial_value) + "\n");
                        } else {
                            shared_state_->mach_semaphores.erase(object);
                            remove_port_object_locked(*shared_state_, object);
                        }
                    } else {
                        shared_state_->mach_semaphores.erase(object);
                    }
                }
            }
        }
        if (port != 0 && !port_already_copied_out) {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            port = shared_state_->mach_namespaces
                       .copyout(process_.pid, port,
                           xnu::ipc::type_mask(xnu::ipc::Right::Send))
                       .value_or(0);
        }
        const std::array<std::uint32_t, 10> reply {
            0x80000012U, // complex + MOVE_SEND_ONCE
            40,
            *local_port,
            0,
            0,
            *message_id + 100,
            1, // one descriptor
            port, // port descriptor name
            0,
            0x00110000U, // MOVE_SEND disposition, port descriptor
        };
        for (std::size_t index = 0; index < reply.size(); ++index) {
            if (!memory_.write32(
                    message_address + static_cast<std::uint32_t>(index * 4U),
                    reply[index])) {
                registers[0] = 0x10004008U;
                return true;
            }
        }
        registers[0] = 0;
        return true;
    }
    return false;
}

} // namespace ilemu

namespace ilemu::port_mig {
using namespace mach_support;
using namespace xnu::mig::mach_port;

bool Rights::handles(std::uint32_t identifier)
{
    return identifier == id(Routine::mach_port_insert_right) ||
           identifier == id(Routine::mach_port_extract_right);
}

Rights::Copyin Rights::copyin_locked(KernelSharedState& state,
    std::uint32_t task, std::uint32_t name, std::uint32_t disposition)
{
    if (!right_for_disposition(disposition))
        return { darwin::mach::invalid_value, { } };
    if (!state.mach_namespaces.contains(task, name))
        return { darwin::mach::invalid_name, { } };
    auto token = mach_transport::PortCopyin { state, task }.capture(
        name, disposition, 28U);
    if (!token)
        return { darwin::mach::invalid_right, { } };
    if (token->right == xnu::ipc::Right::DeadName) {
        if (disposition != darwin::mig_wire::disposition_copy_send)
            static_cast<void>(modify_port_references_locked(
                state, task, name, xnu::ipc::Right::DeadName, -1));
    } else {
        if (disposition >= 16U && disposition <= 18U) {
            if (!consume_moved_right_locked(state, task, name,
                    *source_right_for_disposition(disposition), true))
                return { darwin::mach::invalid_right, { } };
        }
        if (token->right == xnu::ipc::Right::Send)
            ++state.mach_inflight_send_rights[token->object];
        if (disposition == darwin::mig_wire::disposition_make_send)
            static_cast<void>(
                state.mach_port_objects.increment_make_send_count(token->object));
    }
    return { 0U, token };
}

void Rights::release_locked(
    KernelSharedState& state, const Transfer& token, bool installed)
{
    if (token.right == xnu::ipc::Right::Send)
        release_inflight_send_right_locked(state, token.object);
    else if (!installed && token.right == xnu::ipc::Right::Receive)
        terminate_receive_object_locked(state, token.object);
    else if (!installed && token.right == xnu::ipc::Right::SendOnce)
        enqueue_send_once_notification_locked(state, token.object);
}

std::uint32_t Rights::insert_object_locked(KernelSharedState& state,
    std::uint32_t task, std::uint32_t name, const Transfer& token)
{
    if (name == xnu::ipc::null_name || name == xnu::ipc::dead_name)
        return darwin::mach::invalid_value;
    if (token.object == xnu::ipc::null_name ||
        token.object == xnu::ipc::dead_name)
        return darwin::mach::invalid_capability;
    // ipc_right_reverse considers send/receive entries, never send-once aliases.
    std::optional<std::uint32_t> existing_name;
    if (token.right != xnu::ipc::Right::SendOnce) {
        existing_name = state.mach_namespaces.name_for(
            task, token.object, xnu::ipc::Right::Receive);
        if (!existing_name)
            existing_name = state.mach_namespaces.name_for(
                task, token.object, xnu::ipc::Right::Send);
    }
    if (existing_name && *existing_name != name)
        return darwin::mach::right_exists;
    const auto existing = state.mach_namespaces.lookup(task, name);
    if (!existing_name && existing)
        return darwin::mach::name_exists;
    if (!state.mach_port_objects.contains(token.object))
        return darwin::mach::invalid_capability;
    if (existing && token.right == xnu::ipc::Right::Send &&
        existing->user_references[static_cast<std::size_t>(xnu::ipc::Right::Send)] >=
            xnu::ipc::maximum_send_user_references)
        return darwin::mach::user_references_overflow;
    if (!state.mach_namespaces.install(
            task, name, token.object, xnu::ipc::type_mask(token.right)))
        return darwin::mach::resource_shortage;
    if (token.right == xnu::ipc::Right::Receive)
        static_cast<void>(state.mach_port_objects.set_receive_owner(token.object, task));
    return 0U;
}

std::uint32_t Rights::insert_locked(KernelSharedState& state,
    std::uint32_t caller, std::uint32_t target, std::uint32_t name,
    std::uint32_t source, std::uint32_t disposition)
{
    // _kernelrpc_mach_port_insert_right_trap performs copyin before checking
    // the target name; an unsuccessful named copyout destroys the token.
    const auto copied = copyin_locked(state, caller, source, disposition);
    if (copied.error != 0U)
        return copied.error;
    const auto result = insert_object_locked(state, target, name, *copied.right);
    release_locked(state, *copied.right, result == 0U);
    return result;
}

std::optional<std::uint32_t> Rights::dispatch_locked(KernelSharedState& state,
    std::uint32_t object, KernelSharedState::MachMessage& request)
{
    const auto bytes = std::span<const std::byte> { request.bytes };
    const auto identifier = read_little_word(bytes, 20U);
    const auto inserting = identifier == id(Routine::mach_port_insert_right);
    const auto complex = (read_little_word(bytes, 0U) &
                             darwin::mig_wire::message_complex_bit) != 0U;
    const auto error_reply = [&](std::uint32_t error) {
        const std::uint32_t payload[] { 0U, 1U, error };
        return mach_ipc::enqueue_kernel_reply_locked(
            state, request, identifier, payload);
    };
    // MIG validates after transport copyin. Incoming MOVE rights are
    // destroyed on a rejected request, just as in ipc_kobject_server.
    if (bytes.size() != (inserting ? 52U : 40U) || complex != inserting ||
        (inserting && read_little_word(bytes, 24U) != 1U))
        return error_reply(darwin::mig::bad_arguments);
    if (inserting && (read_little_word(bytes, 36U) >> 24U) != 0U)
        return error_reply(darwin::mig::type_error);
    const auto target = state.task_port_pids.find(object);
    if (target == state.task_port_pids.end())
        return error_reply(darwin::mach::invalid_task);
    const auto task = target->second;
    const auto process = state.processes.find(task);
    if (process == state.processes.end() || process->second.exited ||
        !state.mach_namespaces.contains_task(task))
        return error_reply(darwin::mach::invalid_task);
    if (inserting) {
        const auto transfer = std::find_if(request.port_transfers.begin(),
            request.port_transfers.end(), [](const auto& p) {
                return p.descriptor_offset == 28U && !p.array_index;
            });
        const auto disposition = (read_little_word(bytes, 36U) >> 16U) & 0xffU;
        const Transfer token = transfer != request.port_transfers.end()
            ? *transfer
            : Transfer { 28U, 0U, { }, read_little_word(bytes, 28U),
                  right_for_disposition(disposition).value_or(xnu::ipc::Right::DeadName),
                  disposition };
        const auto result = insert_object_locked(
            state, task, read_little_word(bytes, 48U), token);
        if (result == 0U && transfer != request.port_transfers.end()) {
            request.port_transfers.erase(transfer);
            release_locked(state, token, true);
        }
        return error_reply(result);
    }
    const auto name = read_little_word(bytes, 32U);
    const auto disposition = read_little_word(bytes, 36U);
    if (!right_for_disposition(disposition))
        return error_reply(darwin::mach::invalid_value);
    if (name == xnu::ipc::null_name || name == xnu::ipc::dead_name)
        return error_reply(darwin::mach::invalid_right);
    const auto copied = copyin_locked(state, task, name, disposition);
    if (copied.error != 0U)
        return error_reply(copied.error);
    return reply_extracted_locked(state, request, identifier, *copied.right);
}

std::optional<std::uint32_t> Rights::reply_extracted_locked(
    KernelSharedState& state, KernelSharedState::MachMessage& request,
    std::uint32_t identifier, const Transfer& token)
{
    const auto disposition = token.disposition;
    const std::uint32_t payload[] { 1U, token.object, 0U,
        darwin::mig_wire::port_descriptor_metadata(
            darwin::mig_wire::received_port_disposition(disposition)) };
    const auto destination = mach_ipc::enqueue_kernel_reply_locked(state, request,
        identifier, payload, token.right == xnu::ipc::Right::DeadName
            ? std::span<const Transfer> { } : std::span { &token, 1U }, { }, true);
    // enqueue owns produced receive/send-once rights, including on failure.
    // For Send it retains a message reference before releasing this copyin.
    if (token.right == xnu::ipc::Right::Send)
        release_locked(state, token, true);
    return destination;
}
} // namespace ilemu::port_mig

namespace ilemu::port_mig {
std::optional<Rights::SynchronousReply> Rights::try_synchronous_locked(
    AddressSpace& memory, KernelSharedState& state, const ProcessContext& process,
    std::span<const std::uint32_t> registers, std::uint32_t bits,
    std::uint32_t reply_name, std::uint32_t receive_address)
{
    const auto identifier = memory.read32(registers[0] + 20U).value_or(0U);
    const auto inserting = identifier == id(Routine::mach_port_insert_right);
    const auto size = inserting ? 52U : 40U;
    const auto capacity = inserting ? 44U : 48U;
    const auto destination = mach_ipc::validate_task_rpc_locked(memory, state,
        process, registers, bits, reply_name, size, capacity, inserting);
    if (!destination)
        return std::nullopt;
    std::array<std::byte, 52> storage;
    const auto bytes = std::span { storage }.first(size);
    if (!memory.copy_out(registers[0], bytes) || read_little_word(bytes, 16U) != 0U)
        return std::nullopt;
    const auto task = state.task_port_pids.at(destination->task_object);
    const auto target = state.processes.find(task);
    if (target == state.processes.end() || target->second.exited ||
        !state.mach_namespaces.contains_task(task))
        return std::nullopt;
    const auto disposition = inserting
        ? (read_little_word(bytes, 36U) >> 16U) & 0xffU
        : read_little_word(bytes, 36U);
    // Keep destructive transfers and malformed descriptors on ordinary IPC.
    // COPY/MAKE cover the frequent synchronous calls without heap messages.
    if (disposition != 19U && disposition != 20U && disposition != 21U)
        return std::nullopt;
    const auto name = read_little_word(bytes, inserting ? 28U : 32U);
    if (inserting && (read_little_word(bytes, 24U) != 1U ||
                         (read_little_word(bytes, 36U) >> 24U) != 0U ||
                         !mach_transport::PortCopyin { state, process.pid }.capture(
                             name, disposition, 28U)))
        return std::nullopt;
    const auto copied = inserting ? Copyin { }
        : name == xnu::ipc::null_name || name == xnu::ipc::dead_name
            ? Copyin { darwin::mach::invalid_right, { } }
            : copyin_locked(state, task, name, disposition);
    const auto error = inserting
        ? insert_locked(state, process.pid, task, read_little_word(bytes, 48U),
              name, disposition)
        : copied.error;
    const auto complex = !inserting && error == 0U;
    const auto reply_object = destination->reply_object;
    const auto queue = state.mach_queues.find(reply_object);
    if (resolve_name_with_right(state, process.pid, reply_name,
            xnu::ipc::Right::Receive) == reply_object &&
        state.mach_port_objects.contains(reply_object) &&
        queue != state.mach_queues.end() && queue->second.empty() &&
        memory.accessible(receive_address, capacity, MemoryPermission::Write)) {
        std::optional<std::uint32_t> result_name { 0U };
        if (complex) {
            const auto& token = *copied.right;
            result_name = token.right == xnu::ipc::Right::DeadName
                ? xnu::ipc::dead_name
                : mach_transport::PortCopyout { state, process.pid }(
                      token.object, token.right);
        }
        if (result_name) {
            const std::array<std::uint32_t, 4> payload = complex
                ? std::array<std::uint32_t, 4> { 1U, *result_name, 0U,
                      darwin::mig_wire::port_descriptor_metadata(
                          darwin::mig_wire::received_port_disposition(disposition)) }
                : std::array<std::uint32_t, 4> { 0U, 1U, error, 0U };
            if (complex) release_locked(state, *copied.right, true);
            return SynchronousReply { mach_ipc::copyout_kernel_reply_locked(
                memory, state, receive_address, reply_name, reply_object,
                identifier, std::span { payload }.first(complex ? 4U : 3U), complex) };
        }
    }
    // Notification delivery can populate the reply queue during mutation.
    // Never overtake it; retain the same captured result in the queued reply.
    KernelSharedState::MachMessage request;
    request.bytes.assign(bytes.begin(), bytes.end());
    request.reply_object = reply_object;
    request.reply_right = xnu::ipc::Right::SendOnce;
    if (complex) {
        static_cast<void>(reply_extracted_locked(state, request, identifier, *copied.right));
    } else {
        const std::uint32_t payload[] { 0U, 1U, error };
        static_cast<void>(mach_ipc::enqueue_kernel_reply_locked(
            state, request, identifier, payload));
    }
    return SynchronousReply { };
}
} // namespace ilemu::port_mig
