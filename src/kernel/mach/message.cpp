// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Dispatch guest Mach message send and receive operations.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/ipc/mach_msg.c
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/ipc/ipc_mqueue.c

#include "port/rights.hpp"
#include "port/attributes.hpp"
#include "port/membership.hpp"
#include "port/context.hpp"
#include "port/guarded_mig.hpp"
#include "../clock/server.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/darwin_kqueue_abi.hpp"
#include "kernel/darwin_resource_abi.hpp"
#include "kernel/graphics_services_input.hpp"
#include "kernel/kernel.hpp"
#include "kernel/kernel_clock.hpp"
#include "kernel/kernel_iokit.hpp"
#include "kernel/kernel_mach_ipc.hpp"
#include "kernel/kernel_network.hpp"
#include "kernel/mach_clock_abi.hpp"
#include "kernel/mach_descriptor_transport.hpp"
#include "kernel/mach_scheduler_abi.hpp"
#include "kernel/mach_thread_policy_abi.hpp"
#include "kernel/protocol_vproc_contract.hpp"
#include "mach/bootstrap_mig_ids.hpp"
#include "mach/mach_host_mig_ids.hpp"
#include "mach/mach_port_mig_ids.hpp"
#include "mach/mig_wire_abi.hpp"
#include "mach/task_mig_ids.hpp"
#include "mach/thread_act_mig_ids.hpp"
#include "mach/vm_map_mig_ids.hpp"
#include "mach/xnu_mig_adapter.hpp"
#include "media/celestial_volume_protocol.hpp"
#include "media/media_library_service.hpp"
#include "network/darwin_network_abi.hpp"
#include "network/darwin_route_socket.hpp"

#include "host/service_ports.hpp"
#include "host/information.hpp"
#include "vm/allocate.hpp"
#include "vm/deallocate.hpp"
#include "vm/protect.hpp"
#include "port/notifications.hpp"
#include "port/lifecycle.hpp"
#include "port/queries.hpp"
#include "task/enumeration.hpp"
#include "task/information.hpp"
#include "task/trace_memory.hpp"
#include "task/special_ports.hpp"
#include "task/lifecycle.hpp"
#include "thread/policy.hpp"
#include "transport/port_copyin.hpp"
#include "transport/kernel_destination.hpp"
#include "transport/user_header.hpp"
#include "transport/voucher_header.hpp"
#include "transport/ool_copyin.hpp"
#include "transport/copyin_cleanup.hpp"
#include "transport/pseudo_copyout.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "support.hpp"

namespace ilemu {

using namespace mach_support;

namespace {

    bool write_message_words(AddressSpace& memory, std::uint32_t address,
        std::span<const std::uint32_t> words)
    {
        for (std::size_t index = 0; index < words.size(); ++index) {
            if (!memory.write32(address + static_cast<std::uint32_t>(
                                              index * sizeof(std::uint32_t)),
                    words[index])) {
                return false;
            }
        }
        return true;
    }

    bool write_bootstrap_lookup_failure(AddressSpace& memory,
        std::uint32_t message_address, std::uint32_t reply_port,
        std::uint32_t request_id)
    {
        const std::array<std::uint32_t, 9> reply { 18U, 36U, reply_port, 0U, 0U,
            request_id + 100U, 0x00000000U, 0x00000001U, 1102U };
        return write_message_words(memory, message_address, reply);
    }

} // namespace

void CompatibilityKernel::dispatch_mach_message(
    Cpu& cpu, std::optional<std::uint32_t> receive_address)
{
    auto& registers = cpu.registers();
    const auto message_address = registers[0];
    const auto wants_send =
        (registers[1] & darwin::mach_message::option_send) != 0;
    const auto wants_receive =
        (registers[1] & darwin::mach_message::option_receive) != 0;
    if (wants_send && (registers[2] < darwin::mig_wire::message_header_size ||
                          registers[2] % darwin::mig_wire::word_size != 0U)) {
        registers[0] = darwin::mach_message::send_message_too_small;
        return;
    }
    if (!wants_send) {
        if (wants_receive) {
            begin_mach_receive(cpu, receive_address);
        } else {
            // XNU mach_msg_overwrite_trap: with neither operation requested,
            // succeed without copying a header, allocating a message or
            // consuming port rights. Timeout/trailer flags alone do no work.
            registers[0] = 0; // MACH_MSG_SUCCESS
        }
        return;
    }
    auto bits =
        memory_.read32(message_address + darwin::mig_wire::header_bits_offset);
    const auto remote_port = memory_.read32(
        message_address + darwin::mig_wire::header_remote_port_offset);
    const auto local_port = memory_.read32(
        message_address + darwin::mig_wire::header_local_port_offset);
    const auto message_id = memory_.read32(
        message_address + darwin::mig_wire::header_identifier_offset);
    // A send-only mach_msg legitimately carries MACH_PORT_NULL in the local
    // header slot.  Keep the optional reply port distinct from a failed read.
    if (!bits || !remote_port || !local_port || !message_id) {
        registers[0] = 0x1000000eU; // MACH_SEND_INVALID_MEMORY
        return;
    }
    if (wants_send) {
        bits = mach_transport::UserHeader::copyin_bits(
            *bits, *local_port, shared_state_->darwin_abi.mach_message_header);
        if (!bits) {
            // ipc_kmsg_get copies the entire message before checking its
            // header. Preserve that error order without adding a second
            // memory-range lookup to successful optimized RPCs.
            registers[0] = memory_.accessible(
                message_address, registers[2], MemoryPermission::Read)
                ? darwin::mach_message::send_invalid_header
                : darwin::mach_message::send_invalid_data;
            return;
        }
    }
    // Synthetic MIG providers currently encode their reply in the request
    // buffer. A distinct overwrite target must use queued IPC, never silently
    // overwrite a read-only caller's input. Kernel destinations are rejected
    // below until those providers expose separate reply storage.
    const auto separate_receive = wants_receive && receive_address &&
        *receive_address != message_address;
    bool clock_service_request = false;
    if (wants_send && kernel_clock::Server::handles(*message_id)) {
        std::lock_guard lock { shared_state_->mach_mutex };
        const auto object = resolve_name_with_right(
            *shared_state_, process_.pid, *remote_port, xnu::ipc::Right::Send);
        const auto clock =
            object ? kernel_clock::Server::identify(*object, *message_id)
                   : std::nullopt;
        clock_service_request = clock.has_value();
        if (clock && pending_mach_receives_.empty()) {
            const auto result =
                *message_id == xnu::mig::clock::id(
                                   xnu::mig::clock::Routine::clock_alarm)
                    ? kernel_clock::Server::try_alarm_synchronous_locked(
                          memory_, *shared_state_, process_, *clock, registers,
                          *bits, *local_port,
                          receive_address.value_or(message_address))
                    : kernel_clock::Server::try_synchronous_locked(memory_,
                          *shared_state_, process_, *clock, registers, *bits,
                          *local_port,
                          receive_address.value_or(message_address));
            if (result) {
                registers[0] = *result;
                return;
            }
        }
    }
    const auto host_service_request = host_mig::ServicePorts::handles(*message_id);
    if (wants_send && host_service_request && pending_mach_receives_.empty()) {
        std::lock_guard lock { shared_state_->mach_mutex };
        const auto object = resolve_name_with_right(
            *shared_state_, process_.pid, *remote_port, xnu::ipc::Right::Send);
        if (object == mach_task_identity::initial_host_self_name) {
            const auto result = host_mig::ServicePorts::try_synchronous_locked(
                memory_, *shared_state_, process_, registers, *bits, *local_port,
                receive_address.value_or(message_address));
            if (result) {
                registers[0] = *result;
                return;
            }
        }
    }
    const bool special_port_query = *message_id ==
        xnu::mig::task::id(xnu::mig::task::Routine::task_get_special_port);
    if ((task_mig::Enumeration::handles(*message_id) || special_port_query) &&
        pending_mach_receives_.empty()) {
        bool enqueued = false;
        {
            std::lock_guard lock { shared_state_->mach_mutex };
            if (special_port_query) {
                const auto result = task_mig::SpecialPorts::try_synchronous_locked(
                    memory_, *shared_state_, process_, registers, *bits, *local_port,
                    receive_address.value_or(message_address));
                if (result) {
                    registers[0] = *result;
                    return;
                }
            } else {
                enqueued = task_mig::Enumeration::try_synchronous_enqueue_locked(
                    memory_, *shared_state_, process_, registers, *bits, *local_port);
            }
        }
        if (enqueued) {
            begin_mach_receive(cpu, receive_address);
            return;
        }
    }
    const bool vm_memory_request = vm_mig::Allocation::handles(*message_id) ||
                                   vm_mig::Deallocation::handles(*message_id) ||
                                   vm_mig::Protection::handles(*message_id);
    if (vm_memory_request) {
        const std::lock_guard lock { shared_state_->mach_mutex };
        const auto result = vm_mig::Allocation::handles(*message_id)
            ? vm_mig::Allocation::try_synchronous_locked(memory_, *shared_state_,
                  process_, registers, *bits, *local_port,
                  receive_address.value_or(message_address), *message_id)
            : vm_mig::Protection::handles(*message_id)
            ? vm_mig::Protection::try_synchronous_locked(memory_, cpu, *shared_state_,
                  process_, registers, *bits, *local_port,
                  receive_address.value_or(message_address), *message_id)
            : vm_mig::Deallocation::try_synchronous_locked(memory_, cpu, *shared_state_,
                  process_, registers, *bits, *local_port,
                  receive_address.value_or(message_address), *message_id);
        if (result) {
            registers[0] = *result;
            return;
        }
    }
    const bool task_service_request = task_mig::TraceMemory::handles(*message_id) ||
                                      task_mig::Lifecycle::handles(*message_id) ||
                                      task_mig::Enumeration::handles(*message_id) ||
                                      task_mig::SpecialPorts::handles(*message_id);
    const bool thread_policy_request = thread_mig::Policy::handles(*message_id);
    const bool port_query_request = port_mig::Queries::handles(*message_id);
    const bool port_notification_request = port_mig::Notifications::handles(*message_id);
    const bool port_rights_request = port_mig::Rights::handles(*message_id);
    const bool port_lifecycle_request = port_mig::Lifecycle::handles(*message_id);
    const bool port_attributes_request = port_mig::Attributes::handles(*message_id);
    if (port_attributes_request && pending_mach_receives_.empty()) {
        std::optional<port_mig::Attributes::SynchronousReply> reply;
        {
            std::lock_guard lock { shared_state_->mach_mutex };
            reply = port_mig::Attributes::try_synchronous_locked(memory_,
                *shared_state_, process_, registers, *bits, *local_port,
                receive_address.value_or(message_address));
        }
        if (reply) {
            if (reply->received_status) registers[0] = *reply->received_status;
            else begin_mach_receive(cpu, receive_address);
            return;
        }
    }
    if (port_rights_request && pending_mach_receives_.empty()) {
        std::optional<port_mig::Rights::SynchronousReply> reply;
        {
            std::lock_guard lock { shared_state_->mach_mutex };
            reply = port_mig::Rights::try_synchronous_locked(memory_,
                *shared_state_, process_, registers, *bits, *local_port,
                receive_address.value_or(message_address));
        }
        if (reply) {
            if (reply->received_status) registers[0] = *reply->received_status;
            else begin_mach_receive(cpu, receive_address);
            return;
        }
    }
    if (port_lifecycle_request && pending_mach_receives_.empty()) {
        std::optional<port_mig::Lifecycle::SynchronousReply> reply;
        {
            std::lock_guard lock { shared_state_->mach_mutex };
            reply = port_mig::Lifecycle::try_synchronous_locked(memory_,
                *shared_state_, process_, registers, *bits, *local_port,
                receive_address.value_or(message_address));
        }
        if (reply) {
            if (reply->received_status) registers[0] = *reply->received_status;
            else begin_mach_receive(cpu, receive_address);
            return;
        }
    }
    if (port_notification_request && pending_mach_receives_.empty()) {
        std::optional<port_mig::Notifications::SynchronousReply> reply;
        {
            std::lock_guard lock { shared_state_->mach_mutex };
            reply = port_mig::Notifications::try_synchronous_locked(
                memory_, *shared_state_, process_, registers, *bits, *local_port,
                receive_address.value_or(message_address));
        }
        if (reply) {
            if (reply->received_status)
                registers[0] = *reply->received_status;
            else
                begin_mach_receive(cpu, receive_address);
            return;
        }
    }
    if (!separate_receive && port_query_request && pending_mach_receives_.empty()) {
        const MachMessageRequest request { message_address, *bits, *remote_port,
            *local_port, *message_id };
        if (dispatch_mach_port_query_message(cpu, request)) return;
    }
    const auto host_information_request = host_mig::Information::handles(*message_id);
    if (wants_send && host_information_request && pending_mach_receives_.empty()) {
        const std::lock_guard lock { shared_state_->mach_mutex };
        const auto result = host_mig::Information::try_synchronous_locked(
            memory_, *shared_state_, process_, virtual_processor_count_,
            device_model_.memory.usable_ram_bytes, registers, *bits, *local_port,
            receive_address.value_or(message_address));
        if (result) {
            registers[0] = *result;
            return;
        }
    }
    const bool port_membership_request = port_mig::Membership::handles(*message_id);
    if (port_membership_request && pending_mach_receives_.empty()) {
        std::optional<port_mig::Membership::SynchronousReply> reply;
        {
            std::lock_guard lock { shared_state_->mach_mutex };
            reply = port_mig::Membership::try_synchronous_locked(memory_,
                *shared_state_, process_, registers, *bits, *local_port,
                receive_address.value_or(message_address));
        }
        if (reply) {
            if (reply->received_status) registers[0] = *reply->received_status;
            else begin_mach_receive(cpu, receive_address);
            return;
        }
    }
    const bool port_context_request = port_mig::Context::handles(*message_id);
    if (port_context_request && pending_mach_receives_.empty()) {
        const std::lock_guard lock { shared_state_->mach_mutex };
        const auto result = port_mig::Context::try_synchronous_locked(memory_,
            *shared_state_, process_, registers, *bits, *local_port,
            receive_address.value_or(message_address));
        if (result) {
            registers[0] = *result;
            return;
        }
    }
    const bool task_information_request = task_mig::Information::handles(*message_id);
    if (task_information_request) {
        const std::lock_guard lock { shared_state_->mach_mutex };
        const auto result = task_mig::Information::try_synchronous_locked(memory_,
            *shared_state_, process_, registers, *bits, *local_port,
            receive_address.value_or(message_address), task_memory_statistics_query_, task_statistics_query_);
        if (result) {
            registers[0] = *result;
            return;
        }
    }
    // Every optimized RPC above requires an exact voucher-free header. Keep
    // voucher preflight off those hot paths, but before the legacy providers
    // and before ordinary IPC commits any destination or reply rights.
    if (wants_send && (*bits & 0x001f0000U) != 0U) {
        const auto voucher_name = memory_.read32(
            message_address + darwin::mig_wire::header_voucher_offset);
        std::uint32_t error = darwin::mach_message::send_invalid_data;
        if (voucher_name) {
            const std::lock_guard lock { shared_state_->mach_mutex };
            error = mach_transport::VoucherHeader::validate_locked(*shared_state_,
                process_.pid, *remote_port, *local_port, *bits, *voucher_name).error;
        }
        if (error != 0U) {
            registers[0] = memory_.accessible(
                message_address, registers[2], MemoryPermission::Read)
                ? error : darwin::mach_message::send_invalid_data;
            return;
        }
    }
    if (!separate_receive && !clock_service_request && !host_service_request && !host_information_request &&
        !task_service_request && !vm_memory_request && !thread_policy_request && !port_query_request &&
        !port_notification_request && !port_lifecycle_request && !port_rights_request &&
        !port_mig::Guarded::handles(*message_id) && !port_attributes_request && !port_membership_request && !port_context_request && !task_information_request) {
        const auto is_bootstrap_port = [&] {
            std::lock_guard lock { shared_state_->mach_mutex };
            const auto task = mach_task_identity::control_port_locked(*shared_state_, process_);
            const auto bootstrap = mach_task_identity::special_port_locked(*shared_state_, task, 4U);
            return bootstrap != xnu::ipc::null_name &&
                shared_state_->mach_namespaces.resolve(process_.pid, *remote_port) == bootstrap;
        };
        const MachMessageRequest request { message_address, *bits, *remote_port,
            *local_port, *message_id };
        const auto kernel_destination = [&] {
            if (!wants_send) return false;
            const std::lock_guard lock { shared_state_->mach_mutex };
            return mach_transport::KernelDestination::matches_locked(
                *shared_state_, process_.pid, request.remote_port, request.bits);
        }();
        if (kernel_destination && (dispatch_mach_host_message(cpu, request) ||
            dispatch_mach_host_special_port_message(cpu, request) ||
            dispatch_mach_voucher_message(cpu, request) ||
            dispatch_mach_processor_message(cpu, request) ||
            dispatch_mach_exception_ports_message(cpu, request) ||
            dispatch_mach_thread_lifecycle_message(cpu, request) ||
            dispatch_mach_thread_state_message(cpu, request) ||
            dispatch_mach_task_vm_message(cpu, request) ||
            dispatch_mach_rights_message(cpu, request))) {
            return;
        }
        const auto* vproc_log_contract =
            protocol_vproc::contract_for_log_message(*message_id);
        if (vproc_log_contract != nullptr && registers[2] >= 48U) {
            const auto& arguments = xnu::mig::protocol_vproc::log_arguments;
            const auto priority =
                memory_.read32(message_address + arguments[1].request_offset)
                    .value_or(0);
            const auto error =
                memory_.read32(message_address + arguments[2].request_offset)
                    .value_or(0);
            const auto count =
                memory_.read32(message_address + arguments[3].request_count_offset)
                    .value_or(0);
            const auto padded_count = (count + 3U) & ~3U;
            const auto valid_log_shape =
                count != 0U && count <= arguments[3].wire_size &&
                padded_count <= std::numeric_limits<std::uint32_t>::max() -
                                    arguments[3].request_offset &&
                arguments[3].request_offset + padded_count == registers[2];
            if (!valid_log_shape)
                vproc_log_contract = nullptr;
            const auto available = valid_log_shape ? count : 0U;
            std::string message;
            if (available != 0) {
                if (const auto bytes = memory_.read_bytes(
                        message_address + arguments[3].request_offset, available)) {
                    for (const auto byte : *bytes) {
                        const auto character = std::to_integer<unsigned char>(byte);
                        if (character == 0)
                            break;
                        message.push_back(character >= 0x20U && character <= 0x7eU
                                              ? static_cast<char>(character)
                                              : '.');
                    }
                }
            }
            if (vproc_log_contract != nullptr) {
                output_.write(
                    "[launchd-log] pid=" + std::to_string(process_.pid) +
                    " profile=" + std::string { vproc_log_contract->name } +
                    " priority=" + std::to_string(priority) +
                    " error=" + std::to_string(error) +
                    (message.empty() ? std::string { } : " message=" + message) +
                    "\n");
                const std::array<std::uint32_t, 9> reply {
                    18U,
                    36U,
                    *local_port,
                    0U,
                    0U,
                    *message_id + 100U,
                    0x00000000U,
                    0x00000001U,
                    0U,
                };
                registers[0] = write_message_words(memory_, message_address, reply)
                                   ? 0U
                                   : 0x10004008U;
                return;
            }
        }
        if (*message_id ==
                mig_message_id(xnu::mig::bootstrap::Routine::get_self) &&
            registers[3] >= 40U && is_bootstrap_port()) {
            // The root bootstrap provider can query its own job port before it has
            // created a server receive loop. Handle only this structural
            // self-owned case; child requests continue to route to their provider.
            std::uint32_t job_name = 0U;
            {
                std::lock_guard mach_lock { shared_state_->mach_mutex };
                const auto object = shared_state_->mach_namespaces.resolve(
                    process_.pid, *remote_port);
                const auto port = object
                                      ? shared_state_->mach_port_objects.lookup(
                                            *object)
                                      : std::nullopt;
                if (object && port && port->receive_owner == process_.pid) {
                    job_name = shared_state_->mach_namespaces
                                   .copyout(process_.pid, *object,
                                       xnu::ipc::type_mask(
                                           xnu::ipc::Right::Send))
                                   .value_or(0U);
                    if (job_name != 0U) {
                        static_cast<void>(shared_state_->mach_port_objects
                                .increment_make_send_count(*object));
                    }
                }
            }
            if (job_name != 0U) {
                const std::array<std::uint32_t, 10> reply {
                    darwin::mig_wire::message_bits(
                        darwin::mig_wire::disposition_move_send_once, 0U, true),
                    40U,
                    *local_port,
                    0U,
                    0U,
                    *message_id + 100U,
                    1U,
                    job_name,
                    0U,
                    darwin::mig_wire::port_descriptor_metadata(
                        darwin::mig_wire::disposition_move_send),
                };
                registers[0] = write_message_words(memory_, message_address, reply)
                                   ? darwin::mach::success
                                   : darwin::mach_message::receive_invalid_data;
                output_.write("[bootstrap] self job resolved pid=" +
                              std::to_string(process_.pid) +
                              " name=" + std::to_string(job_name) + "\n");
                return;
            }
        }
        if (const auto result = handle_iokit_mach_request(memory_, output_,
                *shared_state_, process_, *message_id, message_address,
                registers[2], registers[3], *remote_port, *local_port,
                IOKitMachCallSite { registers[15], registers[14], registers[7] },
                surface_store_.get())) {
            // A flattened UIKit client may establish its display timing after its
            // event route, with no LayerKit context to provide another callback.
            // The common readiness helper validates process identity, launch
            // intent, prewarm state, event ownership, and live display
            // participation before it can publish a foreground scene, so unrelated
            // IOKit traffic is inert.
            graphics_services_input::activate_resolved_application(
                *shared_state_, process_.pid, scene_coordinator_.get());
            registers[0] = *result;
            return;
        }

        if (*message_id ==
                mig_message_id(xnu::mig::bootstrap::Routine::look_up) &&
            registers[3] >= 40U && is_bootstrap_port()) {
            const auto service_name = memory_.read_c_string(
                message_address +
                    xnu::mig::bootstrap::look_up_arguments[2].request_offset,
                128U);
            if (service_name &&
                *service_name == media_library_service::bootstrap_name &&
                media_library_service::can_serve_empty_catalogue(rootfs_)) {
                std::uint32_t service_name_in_task = 0;
                std::uint32_t service_object = 0;
                {
                    std::lock_guard mach_lock { shared_state_->mach_mutex };
                    const auto generation =
                        shared_state_->bootstrap_service_generations.find(
                            *service_name);
                    if (generation ==
                            shared_state_->bootstrap_service_generations.end() ||
                        generation->second == 0U) {
                        auto service =
                            shared_state_->bootstrap_service_objects.find(
                                *service_name);
                        if (service ==
                            shared_state_->bootstrap_service_objects.end()) {
                            service_object = shared_state_->allocate_mach_object();
                            if (shared_state_->mach_port_objects.create(
                                    service_object)) {
                                shared_state_->mach_queues.try_emplace(
                                    service_object);
                                service =
                                    shared_state_->bootstrap_service_objects
                                        .emplace(*service_name, service_object)
                                        .first;
                            } else {
                                service_object = 0;
                            }
                        } else {
                            service_object = service->second;
                        }
                        if (service_object != 0) {
                            service_name_in_task =
                                shared_state_->mach_namespaces
                                    .copyout(process_.pid, service_object,
                                        xnu::ipc::type_mask(
                                            xnu::ipc::Right::Send))
                                    .value_or(0);
                        }
                    }
                }
                if (service_name_in_task != 0) {
                    const std::array<std::uint32_t, 10> reply {
                        darwin::mig_wire::message_bits(
                            darwin::mig_wire::disposition_move_send_once, 0, true),
                        40U,
                        *local_port,
                        0U,
                        0U,
                        *message_id + 100U,
                        1U,
                        service_name_in_task,
                        0U,
                        darwin::mig_wire::port_descriptor_metadata(
                            darwin::mig_wire::disposition_move_send),
                    };
                    registers[0] =
                        write_message_words(memory_, message_address, reply)
                            ? 0U
                            : 0x10004008U;
                    output_.write("[media] empty-catalogue service resolved pid=" +
                                  std::to_string(process_.pid) + "\n");
                    return;
                }
            }
        }

        if (media_library_service::is_request_identifier(*message_id)) {
            bool media_service = false;
            {
                std::lock_guard mach_lock { shared_state_->mach_mutex };
                const auto destination = shared_state_->mach_namespaces.resolve(
                    process_.pid, *remote_port);
                const auto service = shared_state_->bootstrap_service_objects.find(
                    std::string { media_library_service::bootstrap_name });
                media_service =
                    destination &&
                    service != shared_state_->bootstrap_service_objects.end() &&
                    *destination == service->second;
            }
            auto payload = media_library_service::reply_payload(*message_id)
                               .value_or(std::vector<std::uint32_t> {
                                   0U, 1U, darwin::mig::bad_id });
            const auto reply_size =
                static_cast<std::uint32_t>(darwin::mig_wire::message_header_size +
                                           payload.size() * sizeof(std::uint32_t));
            if (media_service && registers[3] >= reply_size) {
                std::vector<std::uint32_t> reply {
                    darwin::mig_wire::message_bits(
                        darwin::mig_wire::disposition_move_send_once),
                    reply_size,
                    *local_port,
                    0U,
                    0U,
                    *message_id + 100U,
                };
                reply.insert(reply.end(), payload.begin(), payload.end());
                registers[0] = write_message_words(memory_, message_address, reply)
                                   ? 0U
                                   : 0x10004008U;
                output_.write("[media] empty-catalogue request pid=" +
                              std::to_string(process_.pid) +
                              " id=" + std::to_string(*message_id) + "\n");
                return;
            }
        }

        // The host source fallback replaces the hardware render worker only after
        // the guest media service has selected, opened, and prepared its source.
        // Complete the current-source rate operation at that boundary: routing it
        // into the native renderer would synchronously wait for the replaced
        // worker and trigger the service's RPC-timeout recovery. All source and
        // item lifecycle messages continue through the firmware service.
        if (wants_send && local_port && *local_port != xnu::ipc::null_name &&
            registers[2] >= darwin::mig_wire::message_header_size &&
            registers[2] <= 64U * 1024U &&
            registers[3] >= darwin::mig_wire::simple_reply_payload_base) {
            if (const auto bytes = memory_.read_bytes(message_address, registers[2]);
                bytes) {
                const auto property = celestial_volume_protocol::
                    decode_source_float_property_request(*message_id, *bytes);
                if (property && !property->source && property->property == "rate" &&
                    audio_service_->observe_service_source_property(
                        property->source, property->property, property->value)) {
                    constexpr auto reply_size =
                        darwin::mig_wire::simple_reply_payload_base;
                    const std::array<std::uint32_t,
                        reply_size / sizeof(std::uint32_t)>
                        reply {
                            darwin::mig_wire::message_bits(
                                darwin::mig_wire::disposition_move_send_once),
                            reply_size,
                            *local_port,
                            0U,
                            0U,
                            *message_id + 100U,
                            0U,
                            1U,
                            0U,
                        };
                    registers[0] =
                        write_message_words(memory_, message_address, reply)
                            ? darwin::mach::success
                            : darwin::mach_message::receive_invalid_data;
                    output_.line("[audio] source-property source=current key=rate value=" +
                                 std::to_string(property->value) + " completed=host");
                    return;
                }
            }
        }

        if (*message_id ==
                mig_message_id(xnu::mig::bootstrap::Routine::look_up) &&
            process_.pid == 1 && registers[3] >= 36) {
            // launchd probes its own bootstrap namespace before its server
            // receive loop exists. Match XNU/launchd's normal early negative
            // lookup; requests from child processes are routed to PID 1 below.
            registers[0] = write_bootstrap_lookup_failure(
                               memory_, message_address, *local_port, *message_id)
                               ? 0U
                               : 0x10004008U;
            return;
        }
    }
    if (wants_send && registers[2] >= 24 && registers[2] <= 64U * 1024U) {
        auto bytes = memory_.read_bytes(message_address, registers[2]);
        if (!bytes) {
            registers[0] = darwin::mach_message::send_invalid_data;
            return;
        }
        std::uint32_t remote_object = 0;
        const auto caller_header_size =
            memory_
                .read32(message_address + darwin::mig_wire::header_size_offset)
                .value_or(0);
        bool routable = false;
        if (bytes && mach_ipc::normalize_send_header(*bytes, registers[2], *bits)) {
            const auto descriptor_table = mach_transport::preflight_copyin_descriptors(*bytes);
            const auto& descriptors = descriptor_table.descriptors;
            std::unique_lock mach_lock { shared_state_->mach_mutex };
            const auto voucher_name =
                read_little_word(*bytes, darwin::mig_wire::header_voucher_offset);
            const auto voucher = mach_transport::VoucherHeader::validate_locked(
                *shared_state_, process_.pid, *remote_port, *local_port, *bits, voucher_name);
            if (voucher.error != 0U) {
                registers[0] = voucher.error;
                return;
            }
            const auto destination_disposition = *bits & 0xffU;
            const auto destination_right =
                right_for_disposition(destination_disposition);
            const auto destination_source_right =
                source_right_for_disposition(destination_disposition);
            auto destination_object =
                destination_source_right
                    ? resolve_name_with_right(*shared_state_, process_.pid,
                          *remote_port, *destination_source_right)
                    : std::nullopt;
            bool destination_uses_received_type = false;
            // Received MIG headers retain MAKE_SEND/MAKE_SEND_ONCE in some old
            // libSystem paths even though copyout installed the resulting send
            // right. Locally-created messages such as pthread's recycle
            // message, however, legitimately address a receive right with
            // MAKE_SEND. Accept both representations while preserving the
            // resulting right type. A retained MAKE_SEND_ONCE is a use of the
            // already-copied-out SendOnce right and must consume it just like
            // MOVE_SEND_ONCE.
            if (!destination_object && destination_right &&
                destination_source_right != destination_right) {
                destination_object = resolve_name_with_right(*shared_state_,
                    process_.pid, *remote_port, *destination_right);
                destination_uses_received_type = destination_object.has_value();
            }
            if (!destination_object || !destination_right ||
                (*destination_right != xnu::ipc::Right::Send &&
                    *destination_right != xnu::ipc::Right::SendOnce)) {
                destination_object.reset();
            }
            if (destination_object)
                remote_object = *destination_object;
            const auto destination_port =
                destination_object
                    ? shared_state_->mach_port_objects.lookup(remote_object)
                    : std::nullopt;
            const auto clock_service =
                destination_object
                    ? kernel_clock::Server::identify(*destination_object, *message_id)
                    : std::nullopt;
            // Unknown kernel routines still consume the request through
            // ipc_kobject_server and queue a native MIG_BAD_ID reply.
            const auto kernel_service = destination_port &&
                destination_port->kernel_owned && !clock_service;
            // A task-local send name resolves to one global ipc_port
            // object. Port-set membership is retained separately because
            // a receive right may be temporarily in transit.
            routable =
                destination_object &&
                (shared_state_->mach_port_objects.contains(remote_object) ||
                    shared_state_->mach_port_set_links_by_member.contains(
                        remote_object));
            const auto immediate_send_timeout = [&] {
                if (!routable || destination_right == xnu::ipc::Right::SendOnce ||
                    (registers[1] & darwin::mach_message::option_send_timeout) == 0 ||
                    registers[5] != 0)
                    return false;
                const auto port = shared_state_->mach_port_objects.lookup(remote_object);
                const auto depth = shared_state_->mach_message_count_locked(remote_object);
                return port && depth >= port->queue_limit;
            };
            const auto arm_send_possible = [&] {
                if ((registers[1] & darwin::mach_message::option_send_notify) == 0U)
                    return;
                const auto notification = shared_state_->mach_dead_name_notifications.find(
                    std::pair { process_.pid, *remote_port });
                if (notification != shared_state_->mach_dead_name_notifications.end() &&
                    notification->second.send_possible) {
                    notification->second.armed = true;
                    shared_state_->mach_send_possible_armed_destinations.insert(remote_object);
                }
            };
            // A simple COPY_SEND wakeup has no remaining copyin failure point.
            // Avoid a queued-message allocation, but preserve pseudo-copyout:
            // copying the destination back creates one additional Send uref.
            const auto simple_copy_send =
                *bits == darwin::mig_wire::disposition_copy_send &&
                *local_port == xnu::ipc::null_name &&
                read_little_word(*bytes, darwin::mig_wire::header_voucher_offset) == 0U;
            if (simple_copy_send && immediate_send_timeout()) {
                // Admission just validated the live destination under mach_mutex.
                const auto name = shared_state_->mach_namespaces.copyout(
                    process_.pid, remote_object, xnu::ipc::type_mask(xnu::ipc::Right::Send));
                write_little_word(*bytes, darwin::mig_wire::header_bits_offset,
                    darwin::mig_wire::disposition_move_send);
                write_little_word(*bytes, darwin::mig_wire::header_size_offset, registers[2]);
                write_little_word(*bytes, darwin::mig_wire::header_remote_port_offset, name.value_or(0));
                // mach_msg ignores ipc_kmsg_put failure after send timeout.
                mach_transport::PseudoCopyout::write_back(memory_, message_address, *bytes);
                arm_send_possible();
                registers[0] = darwin::mach_message::send_timed_out |
                    (name ? 0U : darwin::mach_message::ipc_space);
                return;
            }
            if (routable) {
                std::optional<std::uint32_t> reply_object;
                std::optional<xnu::ipc::Right> reply_right;
                std::optional<std::uint32_t> voucher_object;
                std::optional<xnu::ipc::Right> voucher_right;
                std::optional<std::uint32_t> destination_send_object;
                std::uint32_t reply_disposition = 0;
                std::uint32_t voucher_disposition = 0;
                KernelSharedState::MachMessage queued;
                auto& port_transfers = queued.port_transfers;
                auto& ool_payloads = queued.ool_payloads;
                auto& ool_port_arrays = queued.ool_port_arrays;
                std::vector<KernelSharedState::MachMessage::PortTransfer>
                    dead_transfers;
                const mach_transport::PortCopyin copyin {
                    *shared_state_, process_.pid };

                const auto reply_name =
                    memory_
                        .read32(message_address +
                                darwin::mig_wire::header_local_port_offset)
                        .value_or(0);
                // NULL and DEAD are literal header values, not namespace
                // lookups (ipc_kmsg_copyin_header's no-reply branch).
                if (reply_name != xnu::ipc::null_name &&
                    reply_name != xnu::ipc::dead_name) {
                    reply_disposition = (*bits >> 8U) & 0xffU;
                    if (const auto transfer = copyin.capture(reply_name,
                            reply_disposition,
                            darwin::mig_wire::header_local_port_offset)) {
                        if (transfer->right == xnu::ipc::Right::DeadName) {
                            dead_transfers.push_back(*transfer);
                        } else {
                            reply_object = transfer->object;
                            reply_right = transfer->right;
                        }
                    } else {
                        registers[0] = darwin::mach_message::send_invalid_reply;
                        return;
                    }
                }

                voucher_disposition = (*bits >> 16U) & 0x1fU;
                if (voucher.object) {
                    voucher_object = voucher.object;
                    voucher_right = xnu::ipc::Right::Send;
                } else if (voucher_disposition != 0U) {
                    // Native copyin canonicalizes a NULL voucher to MOVE_SEND.
                    write_little_word(*bytes, darwin::mig_wire::header_bits_offset,
                        (*bits & ~0x001f0000U) | (17U << 16U));
                }

                // Destination and reply copyin is atomic. Body rights are
                // deliberately excluded: native copyin consumes them in order.
                if (routable) {
                    std::map<std::pair<std::uint32_t, std::uint32_t>,
                        std::uint32_t>
                        moved_references;
                    const auto count_move = [&](std::uint32_t name,
                                                std::uint32_t disposition) {
                        if (disposition != 16U && disposition != 17U &&
                            disposition != 18U) {
                            return true;
                        }
                        const auto source =
                            source_right_for_disposition(disposition);
                        if (!source)
                            return false;
                        ++moved_references[{
                            name, static_cast<std::uint32_t>(*source) }];
                        return true;
                    };
                    if (reply_object &&
                        !count_move(reply_name, reply_disposition)) {
                        routable = false;
                    }
                    if (routable && voucher_object &&
                        !count_move(voucher_name, voucher_disposition)) {
                        routable = false;
                    }
                    for (const auto& transfer : dead_transfers) {
                        if (transfer.disposition == 17U ||
                            transfer.disposition == 18U) {
                            ++moved_references[{ transfer.sender_name,
                                static_cast<std::uint32_t>(
                                    xnu::ipc::Right::DeadName) }];
                        }
                    }
                    const auto destination_move_disposition = *bits & 0xffU;
                    if (routable &&
                        (destination_move_disposition == 17U ||
                            destination_move_disposition == 18U) &&
                        !count_move(
                            *remote_port, destination_move_disposition)) {
                        routable = false;
                    }
                    if (routable) {
                        for (const auto& [key, count] : moved_references) {
                            const auto source =
                                static_cast<xnu::ipc::Right>(key.second);
                            const auto entry =
                                shared_state_->mach_namespaces.lookup(
                                    process_.pid, key.first);
                            if (!entry || (entry->type & xnu::ipc::type_mask(
                                                             source)) == 0) {
                                routable = false;
                                break;
                            }
                            if (source == xnu::ipc::Right::Receive) {
                                if (count != 1U)
                                    routable = false;
                            } else if (entry->user_references[static_cast<
                                           std::size_t>(source)] < count) {
                                routable = false;
                            }
                            if (!routable)
                                break;
                        }
                    }
                    if (!routable) {
                        // MACH_SEND_INVALID_RIGHT; no MOVE right has been
                        // consumed yet.
                        registers[0] = darwin::mach_message::send_invalid_right;
                        return;
                    }
                }

                // Dead names contribute sender urefs but no in-flight right.
                // Canonicalize them only after all reference checks succeed.
                for (const auto& transfer : dead_transfers) {
                    if (transfer.disposition != 19U &&
                        modify_port_references_locked(*shared_state_,
                            process_.pid,
                            transfer.sender_name, xnu::ipc::Right::DeadName, -1) !=
                            darwin::mach::success) {
                        registers[0] = darwin::mach_message::send_invalid_right;
                        return;
                    }
                    if (transfer.array_index) {
                        for (auto& array : ool_port_arrays) {
                            if (array.descriptor_offset ==
                                transfer.descriptor_offset) {
                                array.dead_elements.push_back(*transfer.array_index);
                                break;
                            }
                        }
                    } else {
                        write_little_word(*bytes, transfer.descriptor_offset,
                            xnu::ipc::dead_name);
                    }
                }

                const auto consume_transfer = [&](std::uint32_t name,
                                                  std::uint32_t disposition) {
                    const auto source =
                        source_right_for_disposition(disposition);
                    if (!source)
                        return false;
                    if (disposition != 16U && disposition != 17U &&
                        disposition != 18U) {
                        return true;
                    }
                    return consume_moved_right_locked(
                        *shared_state_, process_.pid, name, *source, true);
                };
                if (routable && reply_object &&
                    !consume_transfer(reply_name, reply_disposition)) {
                    routable = false;
                }
                if (routable && voucher_object &&
                    !consume_transfer(voucher_name, voucher_disposition)) {
                    routable = false;
                }
                const auto destination_move_disposition = *bits & 0xffU;
                const auto consumes_destination_right =
                    destination_move_disposition == 17U ||
                    destination_move_disposition == 18U ||
                    (destination_move_disposition == 21U &&
                        destination_uses_received_type);
                if (routable && consumes_destination_right &&
                    !consume_moved_right_locked(*shared_state_, process_.pid,
                        *remote_port, *destination_right, true)) {
                    routable = false;
                } else if (routable && destination_right &&
                           *destination_right == xnu::ipc::Right::Send) {
                    // COPY/MAKE also create a message-held send right. The
                    // sender can release its namespace entry before receive.
                    destination_send_object = destination_object;
                }
                if (routable) {
                    // The destination participates in atomic header copyin
                    // just like the reply. Old received-type aliases already
                    // own their right and must not manufacture another token.
                    if (!destination_uses_received_type) {
                        if (destination_move_disposition == 20U)
                            static_cast<void>(shared_state_->mach_port_objects
                                .increment_make_send_count(remote_object));
                        else if (destination_move_disposition == 21U)
                            shared_state_->mach_port_objects.make_send_once(remote_object);
                    }
                    const auto retain_inflight = [&](std::uint32_t object,
                                                     xnu::ipc::Right right,
                                                     std::uint32_t
                                                         disposition) {
                        if (right == xnu::ipc::Right::Send) {
                            ++shared_state_->mach_inflight_send_rights[object];
                        }
                        if (disposition == 21U) // MAKE_SEND_ONCE
                            shared_state_->mach_port_objects.make_send_once(object);
                        if (disposition == 20U) { // MAKE_SEND
                            static_cast<void>(shared_state_->mach_port_objects
                                    .increment_make_send_count(object));
                        }
                    };
                    if (reply_object && reply_right) {
                        retain_inflight(
                            *reply_object, *reply_right, reply_disposition);
                    }
                    if (voucher_object && voucher_right) {
                        retain_inflight(*voucher_object, *voucher_right,
                            voucher_disposition);
                    }
                    if (destination_send_object) {
                        ++shared_state_->mach_inflight_send_rights
                              [*destination_send_object];
                    }
                    queued.reply_object = reply_object;
                    queued.reply_right = reply_right;
                    queued.voucher_object = voucher_object;
                    queued.voucher_right = voucher_right;
                    queued.destination_send_object = destination_send_object;
                    if (destination_right == xnu::ipc::Right::SendOnce)
                        queued.destination_send_once_object = destination_object;
                    mach_transport::CopyinCleanup cleanup { *shared_state_,
                        queued, destination_right == xnu::ipc::Right::SendOnce
                                     ? destination_object : std::nullopt };
                    if (descriptor_table.error != 0U) {
                        registers[0] = descriptor_table.error;
                        return;
                    }
                    const mach_transport::OolCopyin ool_copyin { memory_ };
                    bool circular = false;
                    const auto copy_port = [&](std::uint32_t name,
                                               const auto& descriptor,
                                               std::optional<std::uint32_t> element) {
                        if (name == xnu::ipc::null_name)
                            return true;
                        if (name == xnu::ipc::dead_name) {
                            if (element)
                                ool_port_arrays.back().dead_elements.push_back(*element);
                            return true;
                        }
                        const auto transfer = copyin.capture(name,
                            descriptor.disposition(), descriptor.offset, element);
                        if (!transfer)
                            return false;
                        if (transfer->right == xnu::ipc::Right::DeadName) {
                            if (transfer->disposition != 19U &&
                                modify_port_references_locked(*shared_state_,
                                    process_.pid, name, xnu::ipc::Right::DeadName, -1) !=
                                    darwin::mach::success)
                                return false;
                            if (element) {
                                ool_port_arrays.back().dead_elements.push_back(*element);
                            } else {
                                write_little_word(*bytes, descriptor.offset,
                                    xnu::ipc::dead_name);
                            }
                            return true;
                        }
                        if (!consume_transfer(name, transfer->disposition))
                            return false;
                        retain_inflight(transfer->object, transfer->right,
                            transfer->disposition);
                        port_transfers.push_back(*transfer);
                        if (transfer->right == xnu::ipc::Right::Receive &&
                            shared_state_->mach_port_objects.check_circularity(
                                transfer->object, remote_object))
                            circular = true;
                        return true;
                    };
                    const auto reverse = shared_state_->darwin_abi.mach_descriptor_copyin ==
                        DarwinMachDescriptorCopyinAbi::ReverseCompactDescriptors;
                    for (std::size_t index = 0; index < descriptors.size(); ++index) {
                        const auto& descriptor = descriptors[
                            reverse ? descriptors.size() - 1U - index : index];
                        if (descriptor.kind == mach_transport::DescriptorKind::Unknown) {
                            registers[0] = darwin::mach_message::send_invalid_type;
                            return;
                        }
                        bool copied = true;
                        if (descriptor.kind == mach_transport::DescriptorKind::Port) {
                            copied = copy_port(descriptor.address_or_name,
                                descriptor, std::nullopt);
                        } else {
                            auto region = ool_copyin.capture(descriptor);
                            if (region.error != 0U) {
                                registers[0] = region.error;
                                return;
                            }
                            if (descriptor.kind ==
                                mach_transport::DescriptorKind::OutOfLineMemory) {
                                ool_payloads.push_back({
                                    descriptor.offset, std::move(region.bytes) });
                            } else {
                                ool_port_arrays.push_back({ descriptor.offset,
                                    descriptor.count_or_size, { } });
                                for (std::uint32_t element = 0;
                                    element < descriptor.count_or_size; ++element) {
                                    const auto name = read_little_word(region.bytes,
                                        element * darwin::mig_wire::word_size);
                                    if (!copy_port(name, descriptor, element)) {
                                        cleanup.failed_array(descriptor.offset);
                                        copied = false;
                                        break;
                                    }
                                }
                            }
                        }
                        if (!copied) {
                            registers[0] = darwin::mach_message::send_invalid_right;
                            return;
                        }
                    }
                    // Keep payload consumers in physical descriptor order.
                    if (reverse)
                        std::reverse(ool_payloads.begin(), ool_payloads.end());
                    // Complete body copyin first: a later bad descriptor still
                    // wins over circularity. ipc_kmsg_send then destroys the
                    // circular message successfully, before queue admission or
                    // send timeout, and proceeds with any requested receive.
                    if (circular) {
                        cleanup.discard();
                        mach_lock.unlock();
                        if (wants_receive)
                            begin_mach_receive(cpu, receive_address);
                        else
                            registers[0] = darwin::mach::success;
                        return;
                    }
                    // Simple COPY_SEND cannot change the queue during copyin.
                    if (!simple_copy_send && immediate_send_timeout()) {
                        cleanup.commit();
                        mach_transport::PseudoCopyout copyout {
                            *shared_state_, memory_, process_.pid };
                        const auto copyout_error = copyout.restore(queued, *bytes,
                            descriptors, remote_object, *destination_right);
                        mach_transport::PseudoCopyout::write_back(memory_, message_address, *bytes);
                        arm_send_possible();
                        registers[0] = darwin::mach_message::send_timed_out | copyout_error;
                        return;
                    }
                    cleanup.commit();
                    queued.bytes = std::move(*bytes);
                    queued.destination = remote_object;
                    // Simple copyin cannot change the destination under this lock.
                    const auto send_destination = simple_copy_send ? destination_port :
                        shared_state_->mach_port_objects.lookup(remote_object);
                    if (send_destination && !kernel_service && !clock_service &&
                        *destination_right != xnu::ipc::Right::SendOnce &&
                        shared_state_->mach_message_count_locked(remote_object) >=
                            send_destination->queue_limit) {
                        std::optional<std::uint64_t> deadline;
                        if ((registers[1] & darwin::mach_message::option_send_timeout) != 0U)
                            deadline = shared_state_->clock.now() +
                                static_cast<std::uint64_t>(registers[5]) *
                                darwin::mach::scheduler::nanoseconds_per_millisecond;
                        std::array<std::uint32_t, 7> arguments;
                        std::copy_n(registers.begin(), arguments.size(), arguments.begin());
                        // A sleeping send retains its destination independently of the namespace.
                        if (!queued.destination_send_object) {
                            queued.destination_send_object = remote_object;
                            ++shared_state_->mach_inflight_send_rights[remote_object];
                        }
                        auto ticket = shared_state_->mach_send_waiters.wait(std::move(queued),
                            process_.pid, static_cast<std::uint32_t>(cpu.processor_id()),
                            remote_object, deadline);
                        pending_mach_sends_.emplace(cpu.processor_id(), PendingMachSend {
                            std::move(ticket), arguments, receive_address, caller_header_size });
                        process_.waiting_for_events = true;
                        cpu.halt(Dynarmic::HaltReason::UserDefined5);
                        return;
                    }
                    post_mach_send(cpu, std::move(queued), caller_header_size,
                        kernel_service, receive_address, mach_lock);
                    return;
                }
            }
        }

    }
    std::uint32_t unsupported_object = 0;
    std::uint32_t unsupported_owner = 0;
    bool unsupported_known_right = false;
    {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        if (const auto object = shared_state_->mach_namespaces.resolve(
                process_.pid, *remote_port)) {
            unsupported_object = *object;
            unsupported_known_right =
                shared_state_->mach_port_objects.contains(*object);
            if (const auto port_object =
                    shared_state_->mach_port_objects.lookup(*object)) {
                unsupported_owner = port_object->receive_owner;
            }
        }
    }
    // Invalid destination names are an ordinary Mach IPC result. MIG servers
    // also use a null destination when a demux routine returns MIG_NO_REPLY;
    // neither case is an unknown kernel call and neither may halt the guest.
    if (wants_send && registers[2] >= darwin::mig_wire::message_header_size &&
        registers[2] <= 64U * 1024U &&
        (*remote_port == xnu::ipc::null_name || !unsupported_known_right)) {
        registers[0] = darwin::mach_message::send_invalid_destination;
        return;
    }
    std::ostringstream message;
    message << "[mach_msg] unsupported id=" << *message_id
            << mig_message_label(shared_state_->mig_reference, *message_id) << " bits=0x" << std::hex << *bits
            << " header=0x"
            << memory_
                   .read32(
                       message_address + darwin::mig_wire::header_size_offset)
                   .value_or(0xffffffffU)
            << " send=0x" << registers[2] << " remote=0x" << *remote_port
            << " object=0x" << unsupported_object << " known_right=" << std::dec
            << unsupported_known_right << " owner=" << unsupported_owner;
    for (std::size_t offset = 24; offset + 4 <= registers[2]; offset += 4) {
        message << " w" << std::dec << (offset / 4) << "=0x" << std::hex
                << memory_
                       .read32(
                           message_address + static_cast<std::uint32_t>(offset))
                       .value_or(0xffffffffU);
    }
    message << std::dec << '\n';
    output_.write(message.str());
    trace_unknown(cpu, "Mach trap", 31);
    registers[0] = darwin::mach_message::send_invalid_destination;
    return;
}

} // namespace ilemu
