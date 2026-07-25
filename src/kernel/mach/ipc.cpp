#include "ilemu/bootstrap_mig_ids.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kqueue_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
#include "ilemu/graphics_services_input.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/kernel_clock.hpp"
#include "ilemu/kernel_iokit.hpp"
#include "ilemu/kernel_mach_ipc.hpp"
#include "ilemu/kernel_network.hpp"
#include "ilemu/mach_clock_abi.hpp"
#include "ilemu/mach_host_mig_ids.hpp"
#include "ilemu/mach_port_mig_ids.hpp"
#include "ilemu/mach_scheduler_abi.hpp"
#include "ilemu/mach_thread_policy_abi.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/task_mig_ids.hpp"
#include "ilemu/thread_act_mig_ids.hpp"
#include "ilemu/vm_map_mig_ids.hpp"
#include "ilemu/xnu_mig_adapter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "support.hpp"

namespace ilemu {

using namespace mach_support;

bool CompatibilityKernel::deliver_pending_mach(Cpu &cpu) {
  std::lock_guard kernel_lock{mutex_};
  std::lock_guard mach_lock{shared_state_->mach_mutex};
  return deliver_pending_mach_locked(cpu);
}

bool CompatibilityKernel::deliver_pending_mach_locked(Cpu &cpu) {
  const auto pending = pending_mach_receives_.find(cpu.processor_id());
  if (pending == pending_mach_receives_.end())
    return false;

  const auto resolved_receive = shared_state_->mach_namespaces.resolve(
      process_.pid, pending->second.receive_name);
  if (!resolved_receive)
    return false;
  auto queued_port = *resolved_receive;
  auto queue = shared_state_->mach_queues.find(queued_port);
  if (queue == shared_state_->mach_queues.end() || queue->second.empty()) {
    if (const auto port_set = shared_state_->mach_port_sets.find(queued_port);
        port_set != shared_state_->mach_port_sets.end()) {
      queue = shared_state_->mach_queues.end();
      for (const auto member : port_set->second) {
        const auto candidate = shared_state_->mach_queues.find(member);
        if (candidate != shared_state_->mach_queues.end() &&
            !candidate->second.empty()) {
          queued_port = member;
          queue = candidate;
          break;
        }
      }
    }
  }
  if (queue == shared_state_->mach_queues.end() || queue->second.empty()) {
    if (pending->second.deadline &&
        shared_state_->clock.now() >= *pending->second.deadline) {
      cpu.registers()[0] = darwin::mach_message::receive_timed_out;
      pending_mach_receives_.erase(pending);
      process_.waiting_for_events = false;
      cpu.clear_halt();
      return true;
    }
    return false;
  }

  const auto &pending_message = queue->second.front();
  const auto sequence_number =
      shared_state_->mach_port_objects.sequence_number(queued_port).value_or(0);
  auto received = mach_ipc::prepare_received_message(
      pending_message, queued_port, pending->second.options, sequence_number);
  if (!received) {
    auto discarded = std::move(queue->second.front());
    queue->second.pop_front();
    discard_mach_message_rights_locked(*shared_state_, discarded);
    cpu.registers()[0] = 0x10004008U; // MACH_RCV_INVALID_DATA
    pending_mach_receives_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }
  if (received->bytes.size() > pending->second.receive_size) {
    cpu.registers()[0] = 0x10004004U; // MACH_RCV_TOO_LARGE
    pending_mach_receives_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }

  const auto destination_name = shared_state_->mach_namespaces.copyout(
      process_.pid, queued_port,
      xnu792::ipc::type_mask(xnu792::ipc::Right::Receive));
  if (!destination_name) {
    cpu.registers()[0] = 0x10004008U;
    pending_mach_receives_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }
  write_little_word(received->bytes, 12, *destination_name);

  const auto send_bits = read_little_word(pending_message.bytes, 0);
  const auto sender_reply_name = read_little_word(pending_message.bytes, 12);
  if (sender_reply_name != xnu792::ipc::null_name) {
    const auto reply_object =
        pending_message.reply_object
            ? pending_message.reply_object
            : resolve_message_object(*shared_state_, pending_message.sender_pid,
                                     sender_reply_name);
    const auto right = pending_message.reply_right
                           ? pending_message.reply_right
                           : right_for_disposition((send_bits >> 8U) & 0xffU);
    if (right) {
      if (!reply_object) {
        cpu.registers()[0] = 0x10004008U;
        pending_mach_receives_.erase(pending);
        process_.waiting_for_events = false;
        cpu.clear_halt();
        return true;
      }
      const auto reply_name = shared_state_->mach_namespaces.copyout(
          process_.pid, *reply_object, xnu792::ipc::type_mask(*right));
      if (!reply_name) {
        cpu.registers()[0] = 0x10004008U;
        pending_mach_receives_.erase(pending);
        process_.waiting_for_events = false;
        cpu.clear_halt();
        return true;
      }
      write_little_word(received->bytes, 8, *reply_name);
      if (*right == xnu792::ipc::Right::Send) {
        release_inflight_send_right_locked(*shared_state_, *reply_object);
      }
    }
  }

  // Bootstrap lookup bookkeeping belongs to the launchd reply, not the
  // outgoing request. The destination of this queued response is the global
  // reply-port object recorded with the lookup request; the optional
  // reply_object field describes the reply right carried by the message and
  // is not the lookup destination.
  if (pending_message.sender_pid == 1U) {
    const auto service_resolution =
        graphics_services_input::record_bootstrap_reply_locked(
            *shared_state_, pending_message.destination,
            pending_message.port_transfers, process_.pid);
    if (service_resolution.object != 0 &&
        (service_resolution.application_event_port ||
         service_resolution.service_name ==
             graphics_services_input::system_event_service)) {
      output_.write(
          "[input] resolved service=" + service_resolution.service_name +
          " object=" + std::to_string(service_resolution.object) +
          " flushed=" + std::to_string(service_resolution.flushed_events) +
          (service_resolution.application_event_port
               ? " application-event-port"
               : "") +
          "\n");
    }
  }

  if ((send_bits & 0x80000000U) != 0 && pending_message.bytes.size() >= 28U) {
    const auto descriptor_count = read_little_word(pending_message.bytes, 24);
    for (std::uint32_t index = 0; index < descriptor_count; ++index) {
      const auto offset = 28U + static_cast<std::size_t>(index) * 12U;
      if (offset + 12U > pending_message.bytes.size())
        break;
      const auto descriptor_word =
          read_little_word(pending_message.bytes, offset + 8U);
      if ((descriptor_word >> darwin::mig_wire::descriptor_type_shift) != 0) {
        continue;
      }
      const auto sender_name = read_little_word(pending_message.bytes, offset);
      if (sender_name == xnu792::ipc::null_name)
        continue;
      const auto disposition =
          (descriptor_word >> darwin::mig_wire::descriptor_disposition_shift) &
          0xffU;
      const auto captured = std::find_if(
          pending_message.port_transfers.begin(),
          pending_message.port_transfers.end(), [&](const auto &transfer) {
            return transfer.descriptor_offset == offset &&
                   !transfer.array_index;
          });
      const auto right = captured != pending_message.port_transfers.end()
                             ? std::optional{captured->right}
                             : right_for_disposition(disposition);
      if (!right)
        continue;
      const auto object =
          captured != pending_message.port_transfers.end()
              ? std::optional{captured->object}
              : resolve_message_object(*shared_state_,
                                       pending_message.sender_pid, sender_name);
      if (!object) {
        cpu.registers()[0] = 0x10004008U;
        pending_mach_receives_.erase(pending);
        process_.waiting_for_events = false;
        cpu.clear_halt();
        return true;
      }
      const auto receiver_name = shared_state_->mach_namespaces.copyout(
          process_.pid, *object, xnu792::ipc::type_mask(*right));
      if (!receiver_name) {
        cpu.registers()[0] = 0x10004008U;
        pending_mach_receives_.erase(pending);
        process_.waiting_for_events = false;
        cpu.clear_halt();
        return true;
      }
      write_little_word(received->bytes, offset, *receiver_name);
      if (*right == xnu792::ipc::Right::Send) {
        release_inflight_send_right_locked(*shared_state_, *object);
      }
      if (*right == xnu792::ipc::Right::Receive) {
        for (auto &[set_object, members] : shared_state_->mach_port_sets) {
          static_cast<void>(set_object);
          std::erase(members, *object);
        }
        static_cast<void>(shared_state_->mach_port_objects.set_receive_owner(
            *object, process_.pid));
        if (captured == pending_message.port_transfers.end() &&
            pending_message.sender_pid != 0) {
          static_cast<void>(shared_state_->mach_namespaces.remove_type(
              pending_message.sender_pid, sender_name,
              xnu792::ipc::type_mask(xnu792::ipc::Right::Receive)));
        }
      }
    }
  }

  for (const auto &array : pending_message.ool_port_arrays) {
    if (array.descriptor_offset + darwin::mig_wire::descriptor_size >
            received->bytes.size() ||
        array.count > maximum_message_io / darwin::mig_wire::word_size) {
      cpu.registers()[0] = 0x10004008U;
      pending_mach_receives_.erase(pending);
      process_.waiting_for_events = false;
      cpu.clear_halt();
      return true;
    }
    const auto byte_size = array.count * darwin::mig_wire::word_size;
    std::vector<std::byte> names(byte_size);
    for (const auto &transfer : pending_message.port_transfers) {
      if (transfer.descriptor_offset != array.descriptor_offset ||
          !transfer.array_index) {
        continue;
      }
      if (*transfer.array_index >= array.count) {
        cpu.registers()[0] = 0x10004008U;
        pending_mach_receives_.erase(pending);
        process_.waiting_for_events = false;
        cpu.clear_halt();
        return true;
      }
      const auto receiver_name = shared_state_->mach_namespaces.copyout(
          process_.pid, transfer.object,
          xnu792::ipc::type_mask(transfer.right));
      if (!receiver_name) {
        cpu.registers()[0] = 0x10004008U;
        pending_mach_receives_.erase(pending);
        process_.waiting_for_events = false;
        cpu.clear_halt();
        return true;
      }
      write_little_word(names,
                        *transfer.array_index * darwin::mig_wire::word_size,
                        *receiver_name);
      if (transfer.right == xnu792::ipc::Right::Send) {
        release_inflight_send_right_locked(*shared_state_, transfer.object);
      } else if (transfer.right == xnu792::ipc::Right::Receive) {
        for (auto &[set_object, members] : shared_state_->mach_port_sets) {
          static_cast<void>(set_object);
          std::erase(members, transfer.object);
        }
        static_cast<void>(shared_state_->mach_port_objects.set_receive_owner(
            transfer.object, process_.pid));
      }
    }

    std::uint32_t copied_address = 0;
    if (!names.empty()) {
      const auto mapped_size = static_cast<std::uint32_t>(
          (names.size() + AddressSpace::page_size - 1U) &
          ~(static_cast<std::size_t>(AddressSpace::page_size) - 1U));
      const auto free_region =
          find_free_guest_region(memory_, ool_receive_base, mapped_size);
      if (!free_region ||
          !memory_.map(*free_region, mapped_size,
                       MemoryPermission::Read | MemoryPermission::Write) ||
          !memory_.copy_in(*free_region, names)) {
        cpu.registers()[0] = 0x10004008U;
        pending_mach_receives_.erase(pending);
        process_.waiting_for_events = false;
        cpu.clear_halt();
        return true;
      }
      copied_address = *free_region;
    }
    write_little_word(received->bytes, array.descriptor_offset, copied_address);
    output_.write(
        "[mach] ool-ports-copy receiver=" + std::to_string(process_.pid) +
        " count=" + std::to_string(array.count) +
        " address=" + std::to_string(copied_address) + "\n");
  }

  for (const auto &payload : pending_message.ool_payloads) {
    if (payload.descriptor_offset + 4U > received->bytes.size()) {
      cpu.registers()[0] = 0x10004008U; // MACH_RCV_INVALID_DATA
      pending_mach_receives_.erase(pending);
      process_.waiting_for_events = false;
      cpu.clear_halt();
      return true;
    }
    std::uint32_t copied_address = 0;
    if (!payload.bytes.empty()) {
      const auto mapped_size = static_cast<std::uint32_t>(
          (payload.bytes.size() + AddressSpace::page_size - 1U) &
          ~(static_cast<std::size_t>(AddressSpace::page_size) - 1U));
      const auto free_region =
          find_free_guest_region(memory_, ool_receive_base, mapped_size);
      if (!free_region ||
          !memory_.map(*free_region, mapped_size,
                       MemoryPermission::Read | MemoryPermission::Write) ||
          !memory_.copy_in(*free_region, payload.bytes)) {
        cpu.registers()[0] = 0x10004008U;
        pending_mach_receives_.erase(pending);
        process_.waiting_for_events = false;
        cpu.clear_halt();
        return true;
      }
      copied_address = *free_region;
    }
    for (std::size_t byte = 0; byte < 4; ++byte) {
      received->bytes[payload.descriptor_offset + byte] =
          static_cast<std::byte>(copied_address >> (byte * 8U));
    }
    output_.write("[mach] ool-copy receiver=" + std::to_string(process_.pid) +
                  " bytes=" + std::to_string(payload.bytes.size()) +
                  " address=" + std::to_string(copied_address) + "\n");
  }

  if (!mach_ipc::apply_receive_pointer_fixups(
          pending_message, pending->second.message_address, received->bytes)) {
    auto discarded = std::move(queue->second.front());
    queue->second.pop_front();
    discard_mach_message_rights_locked(*shared_state_, discarded);
    cpu.registers()[0] = 0x10004008U; // MACH_RCV_INVALID_DATA
    pending_mach_receives_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }

  queue->second.pop_front();
  output_.write(
      "[mach] deliver sender=" + std::to_string(pending_message.sender_pid) +
      " receiver=" + std::to_string(process_.pid) +
      " port=" + std::to_string(queued_port) +
      " id=" + std::to_string(received->message_id) +
      mig_message_label(received->message_id) +
      " header=" + std::to_string(received->caller_header_size) +
      " bytes=" + std::to_string(received->message_size) +
      " trailer=" + std::to_string(received->trailer_size) + "\n");
  const auto copied =
      memory_.copy_in(pending->second.message_address, received->bytes);
  if (copied) {
    static_cast<void>(
        shared_state_->mach_port_objects.increment_sequence_number(
            queued_port));
  }
  cpu.registers()[0] = copied ? 0U : 0x10004008U; // MACH_RCV_INVALID_DATA
  pending_mach_receives_.erase(pending);
  process_.waiting_for_events = false;
  cpu.clear_halt();
  return true;
}

} // namespace ilemu
