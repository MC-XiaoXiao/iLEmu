// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Dispatch guest Mach message send and receive operations.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/ipc/mach_msg.c
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/ipc/ipc_mqueue.c

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
#include "port/notifications.hpp"
#include "port/queries.hpp"
#include "task/enumeration.hpp"
#include "task/special_ports.hpp"
#include "task/lifecycle.hpp"
#include "thread/policy.hpp"
#include "transport/copyin_cleanup.hpp"
#include "transport/ool_copyin.hpp"
#include "transport/port_copyin.hpp"
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
void CompatibilityKernel::begin_mach_receive(
    Cpu& cpu, std::optional<std::uint32_t> receive_address)
{
    auto& registers = cpu.registers();
    const auto message_address = registers[0];
    const auto timeout_enabled =
        (registers[1] & darwin::mach_message::option_receive_timeout) != 0;
    const auto timeout_milliseconds = registers[5];
    std::optional<std::uint64_t> deadline;
    if (timeout_enabled) {
        const auto interval =
            static_cast<std::uint64_t>(timeout_milliseconds) *
            darwin::mach::scheduler::nanoseconds_per_millisecond;
        const auto now = shared_state_->clock.now();
        deadline = interval > std::numeric_limits<std::uint64_t>::max() - now
                       ? std::numeric_limits<std::uint64_t>::max()
                       : now + interval;
    }
    {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto receive_object =
            resolve_receive_object(*shared_state_, process_.pid, registers[4]);
        if (!receive_object) {
            registers[0] = darwin::mach_message::receive_invalid_name;
            return;
        }
        pending_mach_receives_[cpu.processor_id()] =
            PendingMachReceive { receive_address.value_or(message_address),
                registers[3], registers[4], registers[1], cpu.processor_id(),
                deadline, receive_object,
                shared_state_->mach_port_sets.contains(*receive_object), 0,
                shared_state_->allocate_mach_wait_queue_sequence_locked() };
        process_.waiting_for_events = true;
        if (deliver_pending_mach_locked(cpu, false))
            return;
    }
    if (timeout_enabled && timeout_milliseconds == 0) {
        pending_mach_receives_.erase(cpu.processor_id());
        process_.waiting_for_events = false;
        registers[0] = darwin::mach_message::receive_timed_out;
        return;
    }
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
}
void CompatibilityKernel::post_mach_send(Cpu& cpu,
    KernelSharedState::MachMessage&& queued, std::uint32_t caller_header_size,
    bool kernel_service, std::optional<std::uint32_t> receive_address,
    std::unique_lock<std::mutex>& mach_lock, bool scheduler_completion)
{
    auto& registers = cpu.registers();
    const auto wants_receive =
        (registers[1] & darwin::mach_message::option_receive) != 0;
    auto remote_object = queued.destination;
    if (scheduler_completion && !shared_state_->mach_port_objects.contains(remote_object)) {
        discard_mach_message_rights_locked(*shared_state_, queued);
        mach_lock.unlock();
        if (wants_receive)
            begin_mach_receive(cpu, receive_address);
        else
            registers[0] = darwin::mach::success;
        return;
    }
    std::optional<std::vector<std::byte>> bytes { queued.bytes };
    const auto remote_port = read_little_word(*bytes, 8U);
    const auto message_id = read_little_word(*bytes, 20U);
    const auto reply_object = queued.reply_object;
    const auto& ool_payloads = queued.ool_payloads;
    const auto clock_service =
        kernel_clock::Server::identify(remote_object, message_id);
    std::uint32_t remote_owner = 0;
    std::size_t remote_queue_depth = 0;
    std::optional<std::size_t> local_pending_receiver;
    std::string bootstrap_service_name;
    bool kernel_service_handled = false;
    bool task_suspended = false;
    const auto graphics_event_type =
        graphics_services_input::event_type(*bytes);
    std::optional<std::uint32_t> routed_reply_object;
    std::optional<std::string> service_source_create_path;
    std::optional<std::uint32_t> transferred_receive;
    for (const auto& transfer : queued.port_transfers)
        if (transfer.right == xnu::ipc::Right::Receive)
            transferred_receive = transfer.object;
    const auto bootstrap_lookup =
        message_id == mig_message_id(xnu::mig::bootstrap::Routine::look_up);
    const auto bootstrap_registration =
        message_id ==
        mig_message_id(xnu::mig::bootstrap::Routine::mig_register);
    const auto bootstrap_check_in =
        message_id == mig_message_id(xnu::mig::bootstrap::Routine::check_in);
    std::optional<std::size_t> service_offset;
    if (bootstrap_lookup) {
        service_offset =
            xnu::mig::bootstrap::look_up_arguments[2].request_offset;
    } else if (bootstrap_registration) {
        service_offset =
            xnu::mig::bootstrap::mig_register_arguments[2].request_offset;
    } else if (bootstrap_check_in) {
        service_offset =
            xnu::mig::bootstrap::check_in_arguments[1].request_offset;
    }
    if (service_offset) {
        constexpr std::size_t maximum_service_length = 128;
        for (std::size_t index = 0; index < maximum_service_length &&
                                    *service_offset + index < bytes->size();
            ++index) {
            const auto character = std::to_integer<unsigned char>(
                (*bytes)[*service_offset + index]);
            if (character == 0)
                break;
            if (character < 0x20U || character > 0x7eU) {
                bootstrap_service_name.clear();
                break;
            }
            bootstrap_service_name.push_back(static_cast<char>(character));
        }
    }
    for (const auto& payload : ool_payloads) {
        service_source_create_path =
            celestial_volume_protocol::decode_source_create_path(
                message_id, payload.bytes);
        if (service_source_create_path)
            break;
    }
    if (bootstrap_lookup && !bootstrap_service_name.empty() && reply_object) {
        graphics_services_input::record_bootstrap_lookup_locked(*shared_state_,
            *reply_object, bootstrap_service_name, process_.pid);
    }
    if (bootstrap_check_in && !bootstrap_service_name.empty() && reply_object) {
        graphics_services_input::record_bootstrap_check_in_locked(
            *shared_state_, *reply_object, bootstrap_service_name,
            process_.pid);
    }
    if (bootstrap_registration && !bootstrap_service_name.empty()) {
        graphics_services_input::record_bootstrap_registration_locked(
            *shared_state_, bootstrap_service_name);
    }
    if (bootstrap_check_in && !bootstrap_service_name.empty()) {
        shared_state_->bootstrap_checked_in_services.insert(
            bootstrap_service_name);
    }
    if (const auto process = shared_state_->processes.find(process_.pid);
        process != shared_state_->processes.end() &&
        process->second.core_animation_remote_abi) {
        const auto& profile = *process->second.core_animation_remote_abi;
        const auto exact_transaction =
            profile.is_transaction_message(message_id);
        const auto render_server =
            shared_state_->bootstrap_service_objects.find(
                std::string { graphics_services_input::render_server_service });
        const auto render_server_request =
            profile.render_server_port_rendezvous &&
            render_server != shared_state_->bootstrap_service_objects.end() &&
            render_server->second == remote_object;
        if (exact_transaction || render_server_request) {
            graphics_services_input::
                record_application_remote_scene_commit_locked(*shared_state_,
                    process_.pid, remote_object, scene_coordinator_.get());
        }
    }
    queued.destination = remote_object;
    queued.sender_pid = process_.pid;
    if (const auto sender = shared_state_->processes.find(process_.pid);
        sender != shared_state_->processes.end()) {
        queued.sender_identity_version = sender->second.audit_identity_version;
    }
    if (performance_counters().cpu_source_diagnostics_configured()) {
        queued.host_enqueue_nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
    queued.sender_uid = process_.effective_uid;
    queued.sender_gid = process_.effective_gid;
    routed_reply_object = reply_object;
    if (kernel_service) {
        kernel_service_handled = true;
        const auto destination = port_mig::Queries::handles(message_id)
            ? port_mig::Queries::dispatch_locked(*shared_state_, remote_object, queued)
            : port_mig::Notifications::handles(message_id)
            ? port_mig::Notifications::dispatch_locked(*shared_state_, remote_object, queued)
            : task_mig::Enumeration::handles(message_id)
            ? task_mig::Enumeration::dispatch_locked(*shared_state_, remote_object, queued)
            : task_mig::SpecialPorts::handles(message_id)
            ? task_mig::SpecialPorts::dispatch_locked(*shared_state_, remote_object, queued)
            : task_mig::Lifecycle::handles(message_id)
            ? task_mig::Lifecycle::dispatch_locked(*shared_state_, process_.pid,
                  remote_object, queued, task_suspended)
            : thread_mig::Policy::handles(message_id)
            ? thread_mig::Policy::dispatch_locked(*shared_state_, remote_object, queued,
                  thread_policy_handler_, legacy_thread_policy_handler_, thread_statistics_query_)
            : host_mig::ServicePorts::dispatch_locked(*shared_state_, remote_object, queued);
        remote_object = destination.value_or(0U);
    } else if (clock_service) {
        kernel_service_handled = true;
        const auto destination = kernel_clock::Server::dispatch_locked(
            *shared_state_, *clock_service, queued);
        remote_object = destination.value_or(0U);
    } else {
        shared_state_->enqueue_mach_message_locked(
            remote_object, std::move(queued));
    }
    remote_owner = shared_state_->mach_port_objects.lookup(remote_object)
                       .value_or(xnu::ipc::PortObject { })
                       .receive_owner;
    const auto response_queue = shared_state_->mach_queues.find(remote_object);
    remote_queue_depth = response_queue == shared_state_->mach_queues.end()
                             ? 0U
                             : response_queue->second.size();
    if (remote_owner == process_.pid) {
        local_pending_receiver =
            preferred_pending_mach_receiver_locked(remote_object);
    }
    mach_lock.unlock();

    XnuThreadWakeResult receiver_wake_result { };
    // During event polling the candidate list is already a snapshot. A newly
    // ready receiver must complete copyout on the next generation-driven poll
    // before becoming runnable; waking it here could resume its raw syscall.
    if (!scheduler_completion && local_pending_receiver && thread_wake_handler_) {
        receiver_wake_result = thread_wake_handler_(
            process_.pid, static_cast<std::uint32_t>(*local_pending_receiver));
    } else if (!scheduler_completion && remote_owner != process_.pid && mach_message_wake_handler_ &&
               remote_owner != 0) {
        // The sender's CompatibilityKernel does not own the receiver's
        // pending-mach map. The app-level callback resolves that map
        // and wakes the selected receiver without touching Dynarmic
        // from a different host thread.
        receiver_wake_result =
            mach_message_wake_handler_(remote_owner, remote_object);
    }
    if (receiver_wake_result.preemption_needed && scheduler_preemption_query_ &&
        scheduler_preemption_query_(cpu.processor_id())) {
        cpu.request_guest_preemption();
    }
    if (bytes) {
        if (service_source_create_path && routed_reply_object) {
            audio_service_->observe_service_source_create_request(
                *routed_reply_object, *service_source_create_path);
        }
        if (const auto created =
                celestial_volume_protocol::decode_source_create_reply(
                    message_id, *bytes)) {
            if (const auto path =
                    audio_service_->observe_service_source_create_reply(
                        remote_object, created->source)) {
                output_.line("[audio] source-create source=" +
                             std::to_string(created->source) +
                             " path=" + path->string());
            }
        }
        if (const auto property =
                celestial_volume_protocol::decode_source_float_property_request(
                    message_id, *bytes)) {
            if (audio_service_->observe_service_source_property(
                    property->source, property->property, property->value)) {
                output_.line(
                    "[audio] source-property source=" +
                    (property->source ? std::to_string(*property->source)
                                      : std::string { "current" }) +
                    " key=" + property->property +
                    " value=" + std::to_string(property->value));
            }
        }
        if (const auto update =
                celestial_volume_protocol::decode_reply(message_id, *bytes)) {
            audio_service_->observe_category_volume(
                update->category, update->value);
            output_.write(
                "[audio] category-volume category=" + update->category +
                " value=" + std::to_string(update->value) + "\n");
        }
    }
    if (transferred_receive && output_.enabled("[mach]")) {
        output_.write("[mach] move-receive in-transit port=" +
                      std::to_string(*transferred_receive) +
                      " from=" + std::to_string(process_.pid) + "\n");
    }
    if (!kernel_service_handled && output_.enabled("[mach]"))
        output_.write("[mach] enqueue sender=" + std::to_string(process_.pid) +
                      " port=" + std::to_string(remote_port) +
                      " object=" + std::to_string(remote_object) +
                      " owner=" + std::to_string(remote_owner) +
                      " depth=" + std::to_string(remote_queue_depth) +
                      " id=" + std::to_string(message_id) +
                      mig_message_label(message_id) +
                      (bootstrap_service_name.empty()
                              ? std::string { }
                              : " service=" + bootstrap_service_name) +
                      " caller-header=" + std::to_string(caller_header_size) +
                      " bytes=" + std::to_string(registers[2]) + "\n");
    if (process_.pid != 0) {
        if (graphics_event_type) {
            output_.write(
                "[graphics-event] sender=" + std::to_string(process_.pid) +
                " type=" + std::to_string(*graphics_event_type) +
                " bytes=" + std::to_string(registers[2]) + "\n");
        }
    }
    if (wants_receive) {
        begin_mach_receive(cpu, receive_address);
    } else {
        registers[0] = 0;
    }
    // A synchronous receive clears the CPU wait halt. Install the task AST
    // afterwards so self suspension cannot return to guest instructions.
    if (task_suspended || (kernel_service_handled && scheduler_preemption_query_ &&
            scheduler_preemption_query_(cpu.processor_id())))
        cpu.request_guest_preemption();
    return;
}

bool CompatibilityKernel::complete_pending_mach_send(Cpu& cpu)
{
    const auto found = pending_mach_sends_.find(cpu.processor_id());
    if (found == pending_mach_sends_.end() ||
        !found->second.ticket->ready(shared_state_->clock.now()))
        return false;
    std::unique_lock mach_lock { shared_state_->mach_mutex };
    using State = KernelSharedState::MachSendWaiters::State;
    const auto state =
        found->second.ticket->state.load(std::memory_order_relaxed);
    if (state == State::Waiting &&
        (!found->second.ticket->deadline ||
            shared_state_->clock.now() < *found->second.ticket->deadline))
        return false;
    auto pending = std::move(found->second);
    pending_mach_sends_.erase(found);
    shared_state_->mach_send_waiters.retire(pending.ticket);
    auto& registers = cpu.registers();
    std::copy(
        pending.arguments.begin(), pending.arguments.end(), registers.begin());
    cpu.clear_halt();
    process_.waiting_for_events = false;
    auto& message = pending.ticket->message;
    if (state == State::Waiting || state == State::TimedOut ||
        state == State::Interrupted) {
        const auto descriptors =
            mach_transport::preflight_copyin_descriptors(message.bytes);
        const auto original_name = read_little_word(
            message.bytes, darwin::mig_wire::header_remote_port_offset);
        mach_transport::PseudoCopyout copyout { *shared_state_, memory_,
            process_.pid };
        const auto error =
            copyout.restore(message, message.bytes, descriptors.descriptors,
                pending.ticket->destination, xnu::ipc::Right::Send);
        mach_transport::PseudoCopyout::write_back(
            memory_, pending.arguments[0], message.bytes);
        if (state != State::Interrupted &&
            (pending.arguments[1] & darwin::mach_message::option_send_notify) !=
                0U) {
            const auto notify =
                shared_state_->mach_dead_name_notifications.find(
                    std::pair { process_.pid, original_name });
            if (notify != shared_state_->mach_dead_name_notifications.end() &&
                notify->second.send_possible) {
                notify->second.armed = true;
                shared_state_->mach_send_possible_armed_destinations.insert(
                    pending.ticket->destination);
            }
        }
        registers[0] = (state == State::Interrupted
                           ? darwin::mach_message::send_interrupted
                           : darwin::mach_message::send_timed_out) | error;
        return true;
    }
    if (state == State::Cancelled)
        return true;
    post_mach_send(cpu, std::move(message), pending.caller_header_size, false,
        pending.receive_address, mach_lock, true);
    return !pending_mach_receives_.contains(cpu.processor_id());
}

void KernelSharedState::grant_mach_send_slots_locked(
    std::uint32_t object, std::size_t slots)
{
    if (mach_send_waiters.grant(object, slots, clock.now()))
        mach_queue_generation.note_enqueue(object);
}

void KernelSharedState::cancel_mach_sends_locked(
    std::uint32_t task, std::optional<std::uint32_t> processor)
{
    mach_send_waiters.cancel(task, processor, [&](auto& ticket) {
        discard_mach_message_rights_locked(*this, ticket.message);
        ticket.message = { };
        const auto port = mach_port_objects.lookup(ticket.destination);
        if (port && !mach_ports_being_removed.contains(ticket.destination)) {
            const auto count = mach_message_count_locked(ticket.destination);
            if (count < port->queue_limit)
                grant_mach_send_slots_locked(
                    ticket.destination, port->queue_limit - count);
            notify_send_possible_locked(ticket.destination);
        }
    });
}

} // namespace ilemu
