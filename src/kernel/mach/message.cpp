#include "ilemu/bootstrap_mig_ids.hpp"
#include "ilemu/celestial_volume_protocol.hpp"
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
#include "ilemu/protocol_vproc_profile.hpp"
#include "ilemu/mach_descriptor_transport.hpp"
#include "ilemu/mach_host_mig_ids.hpp"
#include "ilemu/mach_port_mig_ids.hpp"
#include "ilemu/mach_scheduler_abi.hpp"
#include "ilemu/mach_thread_policy_abi.hpp"
#include "ilemu/media_library_service.hpp"
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

bool write_message_words(AddressSpace &memory, std::uint32_t address,
                         std::span<const std::uint32_t> words) {
  for (std::size_t index = 0; index < words.size(); ++index) {
    if (!memory.write32(
            address + static_cast<std::uint32_t>(index * sizeof(std::uint32_t)),
            words[index])) {
      return false;
    }
  }
  return true;
}

} // namespace

void CompatibilityKernel::dispatch_mach_message(Cpu &cpu) {
  auto &registers = cpu.registers();
  const auto message_address = registers[0];
  const auto begin_receive = [&] {
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
      std::lock_guard mach_lock{shared_state_->mach_mutex};
      const auto receive_object = shared_state_->mach_namespaces.resolve(
          process_.pid, registers[4]);
      pending_mach_receives_[cpu.processor_id()] = PendingMachReceive{
          message_address,
          registers[3],
          registers[4],
          registers[1],
          cpu.processor_id(),
          deadline,
          receive_object,
          0,
          shared_state_->allocate_mach_wait_queue_sequence_locked()};
      process_.waiting_for_events = true;
      if (deliver_pending_mach_locked(cpu))
        return;
    }
    if (timeout_enabled && timeout_milliseconds == 0) {
      pending_mach_receives_.erase(cpu.processor_id());
      process_.waiting_for_events = false;
      registers[0] = darwin::mach_message::receive_timed_out;
      return;
    }
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
  };
  const auto wants_send =
      (registers[1] & darwin::mach_message::option_send) != 0;
  const auto wants_receive =
      (registers[1] & darwin::mach_message::option_receive) != 0;
  if (!wants_send && wants_receive) {
    begin_receive();
    return;
  }
  const auto bits =
      memory_.read32(message_address + darwin::mig_wire::header_bits_offset);
  const auto remote_port = memory_.read32(
      message_address + darwin::mig_wire::header_remote_port_offset);
  const auto local_port = memory_.read32(
      message_address + darwin::mig_wire::header_local_port_offset);
  const auto message_id = memory_.read32(
      message_address + darwin::mig_wire::header_identifier_offset);
  if (!bits || !remote_port || !local_port || !message_id) {
    registers[0] = 0x1000000eU; // MACH_SEND_INVALID_MEMORY
    return;
  }
  if (const auto result = handle_clock_mach_request(
          memory_, *shared_state_, process_, *message_id, message_address,
          registers[2], registers[3], *remote_port, *local_port)) {
    registers[0] = *result;
    return;
  }
  const MachMessageRequest request{message_address, *bits, *remote_port,
                                   *local_port, *message_id};
  if (dispatch_mach_host_message(cpu, request) ||
      dispatch_mach_processor_message(cpu, request) ||
      dispatch_mach_port_message(cpu, request) ||
      dispatch_mach_task_enumeration_message(cpu, request) ||
      dispatch_mach_task_info_message(cpu, request) ||
      dispatch_mach_task_exception_message(cpu, request) ||
      dispatch_mach_thread_lifecycle_message(cpu, request) ||
      dispatch_mach_thread_state_message(cpu, request) ||
      dispatch_mach_task_vm_message(cpu, request) ||
      dispatch_mach_rights_message(cpu, request) ||
      dispatch_mach_notification_message(cpu, request)) {
    return;
  }
  const auto *vproc_log_profile =
      protocol_vproc::profile_for_log_message(*message_id);
  if (vproc_log_profile != nullptr && registers[2] >= 48U) {
    const auto &arguments = xnu792::mig::protocol_vproc::log_arguments;
    const auto priority =
        memory_
            .read32(message_address + arguments[1].request_offset)
            .value_or(0);
    const auto error =
        memory_
            .read32(message_address + arguments[2].request_offset)
            .value_or(0);
    const auto count =
        memory_
            .read32(message_address + arguments[3].request_count_offset)
            .value_or(0);
    const auto padded_count = (count + 3U) & ~3U;
    const auto valid_log_shape =
        count != 0U && count <= arguments[3].wire_size &&
        padded_count <= std::numeric_limits<std::uint32_t>::max() -
                            arguments[3].request_offset &&
        arguments[3].request_offset + padded_count == registers[2];
    if (!valid_log_shape)
      vproc_log_profile = nullptr;
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
    if (vproc_log_profile != nullptr) {
      output_.write("[launchd-log] pid=" + std::to_string(process_.pid) +
                    " profile=" + std::string{vproc_log_profile->name} +
                    " priority=" + std::to_string(priority) +
                    " error=" + std::to_string(error) +
                    (message.empty() ? std::string{}
                                     : " message=" + message) +
                    "\n");
      const std::array<std::uint32_t, 9> reply{
          18U,         36U,         *local_port, 0U, 0U, *message_id + 100U,
          0x00000000U, 0x00000001U, 0U,
      };
      registers[0] = write_message_words(memory_, message_address, reply)
                         ? 0U
                         : 0x10004008U;
      return;
    }
  }
  if (const auto result = handle_iokit_mach_request(
          memory_, output_, *shared_state_, process_, *message_id,
          message_address, registers[2], registers[3], *remote_port,
          *local_port,
          IOKitMachCallSite{registers[15], registers[14], registers[7]},
          surface_store_.get())) {
    // A flattened UIKit client may establish its display timing after its
    // event route, with no LayerKit context to provide another callback. The
    // common readiness helper validates process identity, launch intent,
    // prewarm state, event ownership, and live display participation before
    // it can publish a foreground scene, so unrelated IOKit traffic is inert.
    graphics_services_input::activate_resolved_application(
        *shared_state_, process_.pid, scene_coordinator_.get());
    registers[0] = *result;
    return;
  }

  if (*message_id ==
          mig_message_id(xnu792::mig::bootstrap::Routine::look_up) &&
      *remote_port == process_.bootstrap_port && registers[3] >= 40U) {
    const auto service_name = memory_.read_c_string(
        message_address +
            xnu792::mig::bootstrap::look_up_arguments[2].request_offset,
        128U);
    if (service_name &&
        *service_name == media_library_service::bootstrap_name &&
        media_library_service::can_serve_empty_catalogue(rootfs_)) {
      std::uint32_t service_name_in_task = 0;
      std::uint32_t service_object = 0;
      {
        std::lock_guard mach_lock{shared_state_->mach_mutex};
        const auto generation =
            shared_state_->bootstrap_service_generations.find(*service_name);
        if (generation ==
                shared_state_->bootstrap_service_generations.end() ||
            generation->second == 0U) {
          auto service =
              shared_state_->bootstrap_service_objects.find(*service_name);
          if (service == shared_state_->bootstrap_service_objects.end()) {
            service_object = shared_state_->allocate_mach_object();
            if (shared_state_->mach_port_objects.create(service_object)) {
              shared_state_->mach_queues.try_emplace(service_object);
              service = shared_state_->bootstrap_service_objects.emplace(
                  *service_name, service_object).first;
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
                             xnu792::ipc::type_mask(
                                 xnu792::ipc::Right::Send))
                    .value_or(0);
          }
        }
      }
      if (service_name_in_task != 0) {
        const std::array<std::uint32_t, 10> reply{
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
        registers[0] = write_message_words(memory_, message_address, reply)
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
      std::lock_guard mach_lock{shared_state_->mach_mutex};
      const auto destination = shared_state_->mach_namespaces.resolve(
          process_.pid, *remote_port);
      const auto service = shared_state_->bootstrap_service_objects.find(
          std::string{media_library_service::bootstrap_name});
      media_service = destination &&
                      service != shared_state_->bootstrap_service_objects.end() &&
                      *destination == service->second;
    }
    auto payload =
        media_library_service::reply_payload(*message_id)
            .value_or(std::vector<std::uint32_t>{0U, 1U, darwin::mig::bad_id});
    const auto reply_size = static_cast<std::uint32_t>(
        darwin::mig_wire::message_header_size +
        payload.size() * sizeof(std::uint32_t));
    if (media_service && registers[3] >= reply_size) {
      std::vector<std::uint32_t> reply{
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
                    std::to_string(process_.pid) + " id=" +
                    std::to_string(*message_id) + "\n");
      return;
    }
  }

  if (*message_id == mig_message_id(xnu792::mig::bootstrap::Routine::look_up) &&
      process_.pid == 1 && registers[3] >= 36) {
    // launchd probes its own bootstrap namespace before its server
    // receive loop exists. Match XNU/launchd's normal early negative
    // lookup; requests from child processes are routed to PID 1 below.
    const std::array<std::uint32_t, 9> reply{
        18,          36,          *local_port, 0, 0, *message_id + 100,
        0x00000000U, 0x00000001U, 1102,
    };
    for (std::size_t index = 0; index < reply.size(); ++index) {
      if (!memory_.write32(message_address +
                               static_cast<std::uint32_t>(index * 4U),
                           reply[index])) {
        registers[0] = 0x10004008U;
        return;
      }
    }
    registers[0] = 0;
    return;
  }
  if (wants_send && registers[2] >= 24 &&
      registers[2] <= 64U * 1024U) {
    auto bytes = memory_.read_bytes(message_address, registers[2]);
    std::uint32_t remote_object = 0;
    std::uint32_t remote_owner = 0;
    std::size_t remote_queue_depth = 0;
    std::string bootstrap_service_name;
    const auto caller_header_size =
        memory_.read32(message_address + darwin::mig_wire::header_size_offset)
            .value_or(0);
    bool routable = false;
    std::optional<std::uint32_t> graphics_event_type;
    std::optional<std::uint32_t> routed_reply_object;
    std::optional<std::string> service_source_create_path;
    std::optional<std::uint32_t> transferred_receive;
    std::vector<KernelSharedState::MachMessage::OolPayload> ool_payloads;
    std::vector<KernelSharedState::MachMessage::OolPortArray> ool_port_arrays;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ool_deallocations;
    if (bytes && mach_ipc::normalize_send_header(*bytes, registers[2])) {
      graphics_event_type = graphics_services_input::event_type(*bytes);
      const auto bootstrap_lookup =
          *message_id ==
          mig_message_id(xnu792::mig::bootstrap::Routine::look_up);
      const auto bootstrap_registration =
          *message_id ==
          mig_message_id(xnu792::mig::bootstrap::Routine::mig_register);
      const auto bootstrap_check_in =
          *message_id ==
          mig_message_id(xnu792::mig::bootstrap::Routine::check_in);
      std::optional<std::size_t> service_offset;
      if (bootstrap_lookup) {
        service_offset =
            xnu792::mig::bootstrap::look_up_arguments[2].request_offset;
      } else if (bootstrap_registration) {
        service_offset =
            xnu792::mig::bootstrap::mig_register_arguments[2].request_offset;
      } else if (bootstrap_check_in) {
        service_offset =
            xnu792::mig::bootstrap::check_in_arguments[1].request_offset;
      }
      if (service_offset) {
        constexpr std::size_t maximum_service_length = 128;
        for (std::size_t index = 0; index < maximum_service_length &&
                                    *service_offset + index < bytes->size();
             ++index) {
          const auto character =
              std::to_integer<unsigned char>((*bytes)[*service_offset + index]);
          if (character == 0)
            break;
          if (character < 0x20U || character > 0x7eU) {
            bootstrap_service_name.clear();
            break;
          }
          bootstrap_service_name.push_back(static_cast<char>(character));
        }
      }
      const auto descriptors = mach_transport::parse_descriptors(*bytes);
      if (!descriptors) {
        registers[0] = 0x1000000eU;
        return;
      }
      for (const auto &descriptor : *descriptors) {
        if (descriptor.kind ==
            mach_transport::DescriptorKind::OutOfLineMemory) {
          const auto size = descriptor.count_or_size;
          if (size > maximum_ool_payload ||
              (size != 0 && descriptor.address_or_name == 0)) {
            registers[0] = 0x1000000eU;
            return;
          }
          auto payload =
              size == 0 ? std::optional<
                              std::vector<std::byte>>{std::vector<std::byte>{}}
                        : memory_.read_bytes(descriptor.address_or_name, size);
          if (!payload) {
            registers[0] = 0x1000000eU;
            return;
          }
          ool_payloads.push_back(KernelSharedState::MachMessage::OolPayload{
              descriptor.offset, std::move(*payload)});
          if (descriptor.deallocate() && size != 0) {
            ool_deallocations.emplace_back(descriptor.address_or_name, size);
          }
        } else if (descriptor.kind ==
                   mach_transport::DescriptorKind::OutOfLinePorts) {
          if (descriptor.count_or_size >
              maximum_message_io / darwin::mig_wire::word_size) {
            registers[0] = 0x1000000eU;
            return;
          }
          const auto byte_size =
              descriptor.count_or_size * darwin::mig_wire::word_size;
          if ((byte_size != 0 && descriptor.address_or_name == 0) ||
              (byte_size != 0 &&
               !memory_.read_bytes(descriptor.address_or_name, byte_size))) {
            registers[0] = 0x1000000eU;
            return;
          }
          ool_port_arrays.push_back(
              {descriptor.offset, descriptor.count_or_size});
          if (descriptor.deallocate() && byte_size != 0) {
            ool_deallocations.emplace_back(descriptor.address_or_name,
                                           byte_size);
          }
        }
      }
      for (const auto &payload : ool_payloads) {
        service_source_create_path =
            celestial_volume_protocol::decode_source_create_path(
                *message_id, payload.bytes);
        if (service_source_create_path)
          break;
      }
      std::unique_lock mach_lock{shared_state_->mach_mutex};
      const auto destination_disposition = *bits & 0xffU;
      const auto destination_right =
          right_for_disposition(destination_disposition);
      const auto destination_source_right =
          source_right_for_disposition(destination_disposition);
      auto destination_object =
          destination_source_right
              ? resolve_name_with_right(*shared_state_, process_.pid,
                                        *remote_port,
                                        *destination_source_right)
              : std::nullopt;
      bool destination_uses_received_type = false;
      // Received MIG headers retain MAKE_SEND/MAKE_SEND_ONCE in some old
      // libSystem paths even though copyout installed the resulting send
      // right. Locally-created messages such as pthread's recycle message,
      // however, legitimately address a receive right with MAKE_SEND. Accept
      // both representations while preserving the resulting right type. A
      // retained MAKE_SEND_ONCE is a use of the already-copied-out SendOnce
      // right and must consume it just like MOVE_SEND_ONCE.
      if (!destination_object && destination_right &&
          destination_source_right != destination_right) {
        destination_object =
            resolve_name_with_right(*shared_state_, process_.pid, *remote_port,
                                    *destination_right);
        destination_uses_received_type = destination_object.has_value();
      }
      if (!destination_object || !destination_right ||
          (*destination_right != xnu792::ipc::Right::Send &&
           *destination_right != xnu792::ipc::Right::SendOnce)) {
        destination_object.reset();
      }
      if (destination_object)
        remote_object = *destination_object;
      const auto destination_port =
          destination_object
              ? shared_state_->mach_port_objects.lookup(remote_object)
              : std::nullopt;
      if (destination_port && destination_port->kernel_owned) {
        // ipc_kobject_server initializes a generic mig_reply_error_t before
        // looking up the routine. A valid kernel object with no matching
        // demux entry therefore returns MIG_BAD_ID; it is neither an invalid
        // Mach destination nor a message for a user-space server.
        mach_lock.unlock();
        trace_unknown(cpu, "MIG routine", *message_id);
        if (*local_port == xnu792::ipc::null_name) {
          if (wants_receive) {
            begin_receive();
          } else {
            registers[0] = darwin::mach::success;
          }
          return;
        }
        constexpr auto reply_size = darwin::mig_wire::simple_reply_payload_base;
        if (registers[3] < reply_size) {
          registers[0] = darwin::mach_message::receive_invalid_data;
          return;
        }
        const auto reply_disposition =
            darwin::mig_wire::received_port_disposition((*bits >> 8U) & 0xffU);
        const std::array<std::uint32_t, reply_size / sizeof(std::uint32_t)>
            reply{
                darwin::mig_wire::message_bits(reply_disposition),
                reply_size,
                *local_port,
                0U,
                0U,
                *message_id + 100U,
                0U,
                1U,
                darwin::mig::bad_id,
            };
        registers[0] = write_message_words(memory_, message_address, reply)
                           ? darwin::mach::success
                           : darwin::mach_message::receive_invalid_data;
        return;
      }
      // A task-local send name resolves to one global ipc_port
      // object. Port-set membership is retained separately because
      // a receive right may be temporarily in transit.
      const auto is_port_set_member = std::any_of(
          shared_state_->mach_port_sets.begin(),
          shared_state_->mach_port_sets.end(), [&](const auto &port_set) {
            return std::find(port_set.second.begin(), port_set.second.end(),
                             remote_object) != port_set.second.end();
          });
      routable = destination_object &&
                 (shared_state_->mach_port_objects.contains(remote_object) ||
                  is_port_set_member);
      // CoreFoundation deliberately uses a zero-timeout send for run-loop
      // wakeups. XNU rejects a duplicate wakeup once the port's one-message
      // queue is full; allowing it to grow without bound starves the main
      // run loop during periodic display notifications.
      if (routable &&
          (registers[1] & darwin::mach_message::option_send_timeout) != 0 &&
          registers[5] == 0) {
        const auto port =
            shared_state_->mach_port_objects.lookup(remote_object);
        const auto queue = shared_state_->mach_queues.find(remote_object);
        const auto queue_depth = queue == shared_state_->mach_queues.end()
                                     ? 0U
                                     : queue->second.size();
        if (port && queue_depth >= port->queue_limit) {
          registers[0] = darwin::mach_message::send_timed_out;
          return;
        }
      }
      if (routable) {
        std::optional<std::uint32_t> reply_object;
        std::optional<xnu792::ipc::Right> reply_right;
        std::optional<std::uint32_t> destination_send_object;
        std::uint32_t reply_disposition = 0;
        std::vector<KernelSharedState::MachMessage::PortTransfer>
            port_transfers;
        const auto capture = [&](std::uint32_t name, std::uint32_t disposition,
                                 std::uint32_t offset,
                                 std::optional<std::uint32_t> array_index = {})
            -> std::optional<KernelSharedState::MachMessage::PortTransfer> {
          const auto source = source_right_for_disposition(disposition);
          const auto result_right = right_for_disposition(disposition);
          if (!source || !result_right)
            return std::nullopt;
          const auto object = resolve_name_with_right(
              *shared_state_, process_.pid, name, *source);
          if (!object)
            return std::nullopt;
          return KernelSharedState::MachMessage::PortTransfer{
              offset, name, array_index, *object, *result_right, disposition};
        };

        const auto reply_name =
            memory_
                .read32(message_address +
                        darwin::mig_wire::header_local_port_offset)
                .value_or(0);
        if (reply_name != xnu792::ipc::null_name) {
          reply_disposition = (*bits >> 8U) & 0xffU;
          if (const auto transfer = capture(reply_name, reply_disposition, 0)) {
            reply_object = transfer->object;
            reply_right = transfer->right;
          } else {
            // The destination is valid, but the reply right is not present
            // in the sender's IPC namespace (or its disposition is invalid).
            // ipc_kmsg_copyin reports this as a send-right error; treating it
            // as an unknown trap incorrectly terminates otherwise healthy
            // servers which intentionally probe optional rights.
            registers[0] = darwin::mach_message::send_invalid_right;
            return;
          }
        }

        if (routable) {
          for (const auto &descriptor : *descriptors) {
            if (descriptor.kind == mach_transport::DescriptorKind::Port) {
              const auto name = descriptor.address_or_name;
              if (name == xnu792::ipc::null_name)
                continue;
              if (const auto transfer = capture(name, descriptor.disposition(),
                                                descriptor.offset)) {
                port_transfers.push_back(*transfer);
              } else {
                registers[0] = darwin::mach_message::send_invalid_right;
                return;
              }
              continue;
            }
            if (descriptor.kind !=
                mach_transport::DescriptorKind::OutOfLinePorts) {
              continue;
            }
            for (std::uint32_t element = 0; element < descriptor.count_or_size;
                 ++element) {
              const auto name =
                  memory_
                      .read32(descriptor.address_or_name +
                              element * darwin::mig_wire::word_size)
                      .value_or(0);
              if (name == xnu792::ipc::null_name)
                continue;
              if (const auto transfer = capture(name, descriptor.disposition(),
                                                descriptor.offset, element)) {
                port_transfers.push_back(*transfer);
              } else {
                registers[0] = darwin::mach_message::send_invalid_right;
                return;
              }
            }
            if (!routable)
              break;
          }
        }

        // ipc_kmsg_copyin validates every MOVE right before mutating the
        // sender's ipc_space. Do the same preflight here. Without it, a
        // malformed message that repeats one MOVE_SEND/ MOVE_RECEIVE name
        // could consume the first right, fail on the second, and leave the
        // queue/in-flight bookkeeping inconsistent even though no message
        // was enqueued.
        if (routable) {
          std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t>
              moved_references;
          const auto count_move = [&](std::uint32_t name,
                                      std::uint32_t disposition) {
            if (disposition != 16U && disposition != 17U &&
                disposition != 18U) {
              return true;
            }
            const auto source = source_right_for_disposition(disposition);
            if (!source)
              return false;
            ++moved_references[{name, static_cast<std::uint32_t>(*source)}];
            return true;
          };
          if (reply_object &&
              !count_move(reply_name, reply_disposition)) {
            routable = false;
          }
          if (routable) {
            for (const auto &transfer : port_transfers) {
              if (!count_move(transfer.sender_name,
                              transfer.disposition)) {
                routable = false;
                break;
              }
            }
          }
          const auto destination_move_disposition = *bits & 0xffU;
          if (routable && (destination_move_disposition == 17U ||
                           destination_move_disposition == 18U) &&
              !count_move(*remote_port, destination_move_disposition)) {
            routable = false;
          }
          if (routable) {
            for (const auto &[key, count] : moved_references) {
              const auto source = static_cast<xnu792::ipc::Right>(key.second);
              const auto entry = shared_state_->mach_namespaces.lookup(
                  process_.pid, key.first);
              if (!entry ||
                  (entry->type & xnu792::ipc::type_mask(source)) == 0) {
                routable = false;
                break;
              }
              if (source == xnu792::ipc::Right::Receive) {
                if (count != 1U)
                  routable = false;
              } else if (entry->user_references[
                             static_cast<std::size_t>(source)] < count) {
                routable = false;
              }
              if (!routable)
                break;
            }
          }
          if (!routable) {
            // MACH_SEND_INVALID_RIGHT; no MOVE right has been consumed yet.
            registers[0] = darwin::mach_message::send_invalid_right;
            return;
          }
        }

        const auto consume_transfer = [&](std::uint32_t name,
                                          std::uint32_t disposition) {
          const auto source = source_right_for_disposition(disposition);
          if (!source)
            return false;
          if (disposition != 16U && disposition != 17U && disposition != 18U) {
            return true;
          }
          return consume_moved_right_locked(*shared_state_, process_.pid, name,
                                            *source, true);
        };
        if (routable && reply_object &&
            !consume_transfer(reply_name, reply_disposition)) {
          routable = false;
        }
        if (routable) {
          for (const auto &transfer : port_transfers) {
            if (!consume_transfer(transfer.sender_name, transfer.disposition)) {
              routable = false;
              break;
            }
            if (transfer.right == xnu792::ipc::Right::Receive) {
              transferred_receive = transfer.object;
            }
          }
        }
        const auto destination_move_disposition = *bits & 0xffU;
        const auto consumes_destination_right =
            destination_move_disposition == 17U ||
            destination_move_disposition == 18U ||
            (destination_move_disposition == 21U &&
             destination_uses_received_type);
        if (routable && consumes_destination_right &&
            !consume_moved_right_locked(*shared_state_, process_.pid,
                                        *remote_port, *destination_right,
                                        true)) {
          routable = false;
        } else if (routable && destination_move_disposition == 17U &&
                   destination_right &&
                   *destination_right == xnu792::ipc::Right::Send) {
          // MOVE_SEND removes the sender's last ipc_entry reference, but the
          // queued destination still owns that Send right until delivery or
          // discard. Keep it out of no-senders/reclaimer decisions.
          destination_send_object = destination_object;
        }
        if (routable) {
          const auto retain_inflight = [&](std::uint32_t object,
                                           xnu792::ipc::Right right,
                                           std::uint32_t disposition) {
            if (right == xnu792::ipc::Right::Send) {
              ++shared_state_->mach_inflight_send_rights[object];
            }
            if (disposition == 20U) { // MAKE_SEND
              static_cast<void>(
                  shared_state_->mach_port_objects.increment_make_send_count(
                      object));
            }
          };
          if (reply_object && reply_right) {
            retain_inflight(*reply_object, *reply_right, reply_disposition);
          }
          for (const auto &transfer : port_transfers) {
            retain_inflight(transfer.object, transfer.right,
                            transfer.disposition);
          }
          if (destination_send_object) {
            ++shared_state_->mach_inflight_send_rights[
                *destination_send_object];
          }
          if (bootstrap_lookup && !bootstrap_service_name.empty() &&
              reply_object) {
            graphics_services_input::record_bootstrap_lookup_locked(
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
          if (const auto process =
                  shared_state_->processes.find(process_.pid);
              process != shared_state_->processes.end() &&
              process->second.core_animation_remote_profile) {
            const auto &profile =
                *process->second.core_animation_remote_profile;
            const auto exact_transaction =
                profile.is_transaction_message(*message_id);
            const auto render_server =
                shared_state_->bootstrap_service_objects.find(
                    std::string{graphics_services_input::render_server_service});
            const auto render_server_request =
                profile.render_server_port_rendezvous &&
                render_server != shared_state_->bootstrap_service_objects.end() &&
                render_server->second == remote_object;
            if (exact_transaction || render_server_request) {
              graphics_services_input::
                  record_application_remote_scene_commit_locked(
                      *shared_state_, process_.pid, remote_object,
                      scene_coordinator_.get());
            }
          }
          KernelSharedState::MachMessage queued;
          queued.bytes = *bytes;
          queued.destination = remote_object;
          queued.sender_pid = process_.pid;
          queued.sender_uid = process_.effective_uid;
          queued.sender_gid = process_.effective_gid;
          queued.ool_payloads = std::move(ool_payloads);
          queued.ool_port_arrays = std::move(ool_port_arrays);
          queued.reply_object = reply_object;
          routed_reply_object = reply_object;
          queued.reply_right = reply_right;
          queued.destination_send_object = destination_send_object;
          queued.port_transfers = std::move(port_transfers);
          shared_state_->enqueue_mach_message_locked(remote_object,
                                                     std::move(queued));
          remote_owner = shared_state_->mach_port_objects.lookup(remote_object)
                             .value_or(xnu792::ipc::PortObject{})
                             .receive_owner;
          remote_queue_depth = shared_state_->mach_queues[remote_object].size();
        }
      }
    }
    if (routable) {
      if (bytes) {
        if (service_source_create_path && routed_reply_object) {
          audio_service_->observe_service_source_create_request(
              *routed_reply_object, *service_source_create_path);
        }
        if (const auto created =
                celestial_volume_protocol::decode_source_create_reply(
                    *message_id, *bytes)) {
          if (const auto path =
                  audio_service_->observe_service_source_create_reply(
                      remote_object, created->source)) {
            output_.line("[audio] source-create source=" +
                         std::to_string(created->source) + " path=" +
                         path->string());
          }
        }
        if (const auto property =
                celestial_volume_protocol::
                    decode_source_float_property_request(*message_id,
                                                         *bytes)) {
          if (audio_service_->observe_service_source_property(
                  property->source, property->property, property->value)) {
            output_.line("[audio] source-property source=" +
                         std::to_string(property->source) + " key=" +
                         property->property + " value=" +
                         std::to_string(property->value));
          }
        }
        if (const auto update = celestial_volume_protocol::decode_reply(
                *message_id, *bytes)) {
          audio_service_->observe_category_volume(update->category,
                                                   update->value);
          output_.write("[audio] category-volume category=" +
                        update->category + " value=" +
                        std::to_string(update->value) + "\n");
        }
      }
      for (const auto &[address, size] : ool_deallocations) {
        static_cast<void>(memory_.unmap(address, size));
      }
      if (transferred_receive) {
        output_.write("[mach] move-receive in-transit port=" +
                      std::to_string(*transferred_receive) +
                      " from=" + std::to_string(process_.pid) + "\n");
      }
      output_.write("[mach] enqueue sender=" + std::to_string(process_.pid) +
                    " port=" + std::to_string(*remote_port) +
                    " object=" + std::to_string(remote_object) +
                    " owner=" + std::to_string(remote_owner) +
                    " depth=" + std::to_string(remote_queue_depth) +
                    " id=" + std::to_string(*message_id) +
                    mig_message_label(*message_id) +
                    (bootstrap_service_name.empty()
                         ? std::string{}
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
        begin_receive();
      } else {
        registers[0] = 0;
      }
      return;
    }
  }
  std::uint32_t unsupported_object = 0;
  std::uint32_t unsupported_owner = 0;
  bool unsupported_known_right = false;
  {
    std::lock_guard mach_lock{shared_state_->mach_mutex};
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
  if (wants_send &&
      registers[2] >= darwin::mig_wire::message_header_size &&
      registers[2] <= 64U * 1024U &&
      (*remote_port == xnu792::ipc::null_name || !unsupported_known_right)) {
    registers[0] = darwin::mach_message::send_invalid_destination;
    return;
  }
  std::ostringstream message;
  message << "[mach_msg] unsupported id=" << *message_id
          << mig_message_label(*message_id) << " bits=0x" << std::hex << *bits
          << " header=0x"
          << memory_
                 .read32(message_address + darwin::mig_wire::header_size_offset)
                 .value_or(0xffffffffU)
          << " send=0x" << registers[2] << " remote=0x" << *remote_port
          << " object=0x" << unsupported_object << " known_right=" << std::dec
          << unsupported_known_right << " owner=" << unsupported_owner;
  for (std::size_t offset = 24; offset + 4 <= registers[2]; offset += 4) {
    message << " w" << std::dec << (offset / 4) << "=0x" << std::hex
            << memory_
                   .read32(message_address + static_cast<std::uint32_t>(offset))
                   .value_or(0xffffffffU);
  }
  message << std::dec << '\n';
  output_.write(message.str());
  trace_unknown(cpu, "Mach trap", 31);
  registers[0] = darwin::mach_message::send_invalid_destination;
  return;
}

} // namespace ilemu
