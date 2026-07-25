#include "ilemu/bootstrap_mig_ids.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kqueue_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
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

namespace mach_support {

static_assert(
    darwin::mach::thread_policy::policy_set_message ==
    mig_message_id(xnu792::mig::thread_act::Routine::thread_policy_set));

std::string mig_message_label(std::uint32_t identifier) {
  const auto routine = xnu792::mig::lookup_routine(identifier);
  if (!routine)
    return {};
  return " mig=" + std::string{routine->subsystem_name} + '.' +
         std::string{routine->routine_name};
}

bool guest_region_overlaps(const AddressSpace &memory, std::uint32_t address,
                           std::uint32_t size) {
  if (size == 0 ||
      size - 1U > std::numeric_limits<std::uint32_t>::max() - address) {
    return true;
  }
  for (std::uint64_t offset = 0; offset < size;
       offset += AddressSpace::page_size) {
    if (memory.mapped(address + static_cast<std::uint32_t>(offset))) {
      return true;
    }
  }
  return false;
}

std::optional<std::uint32_t> find_free_guest_region(const AddressSpace &memory,
                                                    std::uint32_t start,
                                                    std::uint32_t size) {
  auto candidate = start & ~(AddressSpace::page_size - 1U);
  while (size != 0 &&
         size - 1U <= std::numeric_limits<std::uint32_t>::max() - candidate) {
    if (!guest_region_overlaps(memory, candidate, size))
      return candidate;
    if (candidate >
        std::numeric_limits<std::uint32_t>::max() - AddressSpace::page_size) {
      break;
    }
    candidate += AddressSpace::page_size;
  }
  return std::nullopt;
}

std::uint32_t read_little_word(std::span<const std::byte> bytes,
                               std::size_t offset) {
  if (offset + sizeof(std::uint32_t) > bytes.size())
    return 0;
  std::uint32_t value = 0;
  for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
    value |= std::to_integer<std::uint32_t>(bytes[offset + byte])
             << (byte * 8U);
  }
  return value;
}

void write_little_word(std::span<std::byte> bytes, std::size_t offset,
                       std::uint32_t value) {
  if (offset + sizeof(value) > bytes.size())
    return;
  for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
    bytes[offset + byte] = static_cast<std::byte>(value >> (byte * 8U));
  }
}

std::optional<xnu792::ipc::Right>
right_for_disposition(std::uint32_t disposition) {
  switch (disposition) {
  case 16:
    return xnu792::ipc::Right::Receive; // MOVE_RECEIVE
  case 17:                              // MOVE_SEND
  case 19:                              // COPY_SEND
  case 20:
    return xnu792::ipc::Right::Send; // MAKE_SEND
  case 18:                           // MOVE_SEND_ONCE
  case 21:
    return xnu792::ipc::Right::SendOnce; // MAKE_SEND_ONCE
  default:
    return std::nullopt;
  }
}

std::optional<xnu792::ipc::Right>
source_right_for_disposition(std::uint32_t disposition) {
  switch (disposition) {
  case 16:
    return xnu792::ipc::Right::Receive; // MOVE_RECEIVE
  case 17:
    return xnu792::ipc::Right::Send; // MOVE_SEND
  case 18:
    return xnu792::ipc::Right::SendOnce; // MOVE_SEND_ONCE
  case 19:
    return xnu792::ipc::Right::Send; // COPY_SEND
  case 20:                           // MAKE_SEND
  case 21:
    return xnu792::ipc::Right::Receive; // MAKE_SEND_ONCE
  default:
    return std::nullopt;
  }
}

std::optional<std::uint32_t>
target_task_for_port(const KernelSharedState &state, std::uint32_t caller,
                     std::uint32_t task_name) {
  const auto task_object = state.mach_namespaces.resolve(caller, task_name);
  if (!task_object)
    return std::nullopt;
  const auto task = state.task_port_pids.find(*task_object);
  return task == state.task_port_pids.end()
             ? std::nullopt
             : std::optional<std::uint32_t>{task->second};
}

std::optional<std::pair<std::uint32_t, std::uint32_t>>
find_thread_owner(const KernelSharedState &state, std::uint32_t object) {
  for (const auto &[pid, threads] : state.task_thread_port_objects) {
    for (const auto &[slot, thread_object] : threads) {
      if (thread_object == object)
        return std::pair{pid, slot};
    }
  }
  return std::nullopt;
}

std::optional<std::uint32_t>
resolve_name_with_right(const KernelSharedState &state, std::uint32_t task,
                        std::uint32_t name, xnu792::ipc::Right right) {
  const auto entry = state.mach_namespaces.lookup(task, name);
  if (!entry || (entry->type & xnu792::ipc::type_mask(right)) == 0) {
    return std::nullopt;
  }
  return entry->object;
}

std::optional<std::uint32_t>
resolve_message_object(const KernelSharedState &state, std::uint32_t sender,
                       std::uint32_t name) {
  // Kernel-originated notification messages store global object identifiers
  // directly. Every user-originated message must cross an ipc_space lookup.
  if (sender == 0)
    return name;
  return state.mach_namespaces.resolve(sender, name);
}

bool enqueue_no_senders_notification_locked(KernelSharedState &state,
                                            std::uint32_t object) {
  const auto key = std::pair{object, mach_notify_no_senders};
  const auto request = state.mach_notifications.find(key);
  const auto port_object = state.mach_port_objects.lookup(object);
  if (request == state.mach_notifications.end() || !port_object ||
      request->second.notify_object == xnu792::ipc::null_name ||
      state.mach_namespaces.right_reference_count(
          object, xnu792::ipc::Right::Send) != 0 ||
      state.mach_inflight_send_rights[object] != 0 ||
      port_object->make_send_count < request->second.sync) {
    return false;
  }

  KernelSharedState::MachMessage message;
  message.bytes.resize(36);
  write_little_word(message.bytes, 0, 18); // MOVE_SEND_ONCE
  write_little_word(message.bytes, 4,
                    static_cast<std::uint32_t>(message.bytes.size()));
  write_little_word(message.bytes, 8, request->second.notify_object);
  write_little_word(message.bytes, 20, mach_notify_no_senders);
  write_little_word(message.bytes, 24, 0); // native NDR
  write_little_word(message.bytes, 28, 1); // little-endian NDR
  write_little_word(message.bytes, 32, port_object->make_send_count);
  message.destination = request->second.notify_object;
  state.mach_queues[message.destination].push_back(std::move(message));
  state.mach_notifications.erase(request);
  return true;
}

void enqueue_dead_name_notification_locked(KernelSharedState &state,
                                           std::uint32_t notify_object,
                                           std::uint32_t dead_name) {
  KernelSharedState::MachMessage message;
  message.bytes.resize(36);
  write_little_word(message.bytes, 0, 18); // MOVE_SEND_ONCE
  write_little_word(message.bytes, 4,
                    static_cast<std::uint32_t>(message.bytes.size()));
  write_little_word(message.bytes, 8, notify_object);
  write_little_word(message.bytes, 20, mach_notify_dead_name);
  write_little_word(message.bytes, 24, 0);
  write_little_word(message.bytes, 28, 1);
  write_little_word(message.bytes, 32, dead_name);
  message.destination = notify_object;
  state.mach_queues[notify_object].push_back(std::move(message));
}

void enqueue_port_deleted_notification_locked(KernelSharedState &state,
                                              std::uint32_t notify_object,
                                              std::uint32_t deleted_name) {
  KernelSharedState::MachMessage message;
  message.bytes.resize(36);
  write_little_word(message.bytes, 0, 18); // MOVE_SEND_ONCE
  write_little_word(message.bytes, 4,
                    static_cast<std::uint32_t>(message.bytes.size()));
  write_little_word(message.bytes, 8, notify_object);
  write_little_word(message.bytes, 20, mach_notify_port_deleted);
  write_little_word(message.bytes, 24, 0);
  write_little_word(message.bytes, 28, 1);
  write_little_word(message.bytes, 32, deleted_name);
  message.destination = notify_object;
  state.mach_queues[notify_object].push_back(std::move(message));
}

void enqueue_send_once_notification_locked(KernelSharedState &state,
                                           std::uint32_t object) {
  KernelSharedState::MachMessage message;
  message.bytes.resize(24);
  write_little_word(message.bytes, 0, 18); // MOVE_SEND_ONCE
  write_little_word(message.bytes, 4,
                    static_cast<std::uint32_t>(message.bytes.size()));
  write_little_word(message.bytes, 8, object);
  write_little_word(message.bytes, 20, mach_notify_send_once);
  message.destination = object;
  state.mach_queues[object].push_back(std::move(message));
}

void enqueue_port_destroyed_notification_locked(KernelSharedState &state,
                                                std::uint32_t notify_object,
                                                std::uint32_t receive_object) {
  KernelSharedState::MachMessage message;
  message.bytes.resize(40);
  write_little_word(message.bytes, 0, 0x80000012U);
  write_little_word(message.bytes, 4,
                    static_cast<std::uint32_t>(message.bytes.size()));
  write_little_word(message.bytes, 8, notify_object);
  write_little_word(message.bytes, 20, mach_notify_port_destroyed);
  write_little_word(message.bytes, 24, 1); // one port descriptor
  write_little_word(message.bytes, 28, receive_object);
  write_little_word(message.bytes, 36, 0x00100000U); // MOVE_RECEIVE
  message.destination = notify_object;
  state.mach_queues[notify_object].push_back(std::move(message));
}

void discard_mach_message_rights_locked(
    KernelSharedState &state, const KernelSharedState::MachMessage &message);

void remove_port_object_locked(KernelSharedState &state, std::uint32_t object) {
  for (auto &[set_object, members] : state.mach_port_sets) {
    static_cast<void>(set_object);
    std::erase(members, object);
  }
  state.mach_port_sets.erase(object);
  if (auto queue = state.mach_queues.find(object);
      queue != state.mach_queues.end()) {
    auto discarded = std::move(queue->second);
    state.mach_queues.erase(queue);
    for (const auto &message : discarded) {
      discard_mach_message_rights_locked(state, message);
    }
    // A send-once notification aimed back at the dying object is itself
    // undeliverable and must not recreate the receive queue.
    state.mach_queues.erase(object);
  }
  static_cast<void>(state.mach_port_objects.erase(object));
  state.mach_inflight_send_rights.erase(object);
  state.task_port_pids.erase(object);
  for (auto task = state.task_thread_port_objects.begin();
       task != state.task_thread_port_objects.end();) {
    std::erase_if(task->second, [object](const auto &entry) {
      return entry.second == object;
    });
    if (task->second.empty()) {
      task = state.task_thread_port_objects.erase(task);
    } else {
      ++task;
    }
  }
  state.task_special_ports.erase(object);
  state.mach_semaphores.erase(object);
  state.mach_timers.erase(object);
  state.mach_memory_entries.erase(object);
  state.iokit_iterators.erase(object);
  state.iokit_connections.erase(object);
  state.iokit_services.erase(object);
  state.iokit_interest_notifications.erase(object);
  if (state.mobile_framebuffer_service == object) {
    state.mobile_framebuffer_service = 0;
  }
  if (state.wifi_service == object) {
    state.wifi_service = 0;
  }
  if (state.wifi_interface_service == object) {
    state.wifi_interface_service = 0;
  }
  state.mach_notifications.erase(std::pair{object, mach_notify_port_destroyed});
  state.mach_notifications.erase(std::pair{object, mach_notify_no_senders});
}

void cancel_dead_name_notification_locked(KernelSharedState &state,
                                          std::uint32_t task,
                                          std::uint32_t name) {
  const auto key = std::pair{task, name};
  const auto request = state.mach_dead_name_notifications.find(key);
  if (request == state.mach_dead_name_notifications.end())
    return;
  if (request->second.notify_object != xnu792::ipc::null_name) {
    enqueue_port_deleted_notification_locked(
        state, request->second.notify_object, name);
  }
  state.mach_dead_name_notifications.erase(request);
}

bool consume_moved_right_locked(KernelSharedState &state, std::uint32_t task,
                                std::uint32_t name, xnu792::ipc::Right right,
                                bool remains_in_flight) {
  const auto entry = state.mach_namespaces.lookup(task, name);
  if (!entry || (entry->type & xnu792::ipc::type_mask(right)) == 0) {
    return false;
  }
  if (right == xnu792::ipc::Right::Receive) {
    if (!state.mach_namespaces.remove_type(task, name,
                                           xnu792::ipc::type_mask(right))) {
      return false;
    }
    for (auto &[set_object, members] : state.mach_port_sets) {
      static_cast<void>(set_object);
      std::erase(members, entry->object);
    }
    static_cast<void>(
        state.mach_port_objects.set_receive_owner(entry->object, 0));
    return true;
  }
  if (right != xnu792::ipc::Right::Send &&
      right != xnu792::ipc::Right::SendOnce) {
    return false;
  }
  if (!state.mach_namespaces.modify_references(task, name, right, -1)) {
    return false;
  }
  if (!state.mach_namespaces.contains(task, name)) {
    cancel_dead_name_notification_locked(state, task, name);
  }
  if (right == xnu792::ipc::Right::Send && !remains_in_flight) {
    static_cast<void>(
        enqueue_no_senders_notification_locked(state, entry->object));
  }
  return true;
}

void terminate_receive_object_locked(KernelSharedState &state,
                                     std::uint32_t object) {
  for (auto &[set_object, members] : state.mach_port_sets) {
    static_cast<void>(set_object);
    std::erase(members, object);
  }

  const auto destroyed_key = std::pair{object, mach_notify_port_destroyed};
  const auto destroyed_request = state.mach_notifications.find(destroyed_key);
  if (destroyed_request != state.mach_notifications.end() &&
      destroyed_request->second.notify_object != xnu792::ipc::null_name) {
    enqueue_port_destroyed_notification_locked(
        state, destroyed_request->second.notify_object, object);
    state.mach_notifications.erase(destroyed_request);
    // The port remains active without a receiver until the MOVE_RECEIVE
    // descriptor is copied out by the notification receiver.
    static_cast<void>(state.mach_port_objects.set_receive_owner(object, 0));
    return;
  }
  state.mach_notifications.erase(destroyed_key);

  static_cast<void>(state.mach_namespaces.mark_object_dead(object));
  for (auto request = state.mach_dead_name_notifications.begin();
       request != state.mach_dead_name_notifications.end();) {
    if (request->second.target_object != object) {
      ++request;
      continue;
    }
    const auto task = request->first.first;
    const auto name = request->first.second;
    // XNU adds one dead-name uref for every generated notification.
    static_cast<void>(state.mach_namespaces.modify_references(
        task, name, xnu792::ipc::Right::DeadName, 1));
    enqueue_dead_name_notification_locked(state, request->second.notify_object,
                                          name);
    request = state.mach_dead_name_notifications.erase(request);
  }
  remove_port_object_locked(state, object);
}

void release_inflight_send_right_locked(KernelSharedState &state,
                                        std::uint32_t object) {
  const auto inflight = state.mach_inflight_send_rights.find(object);
  if (inflight == state.mach_inflight_send_rights.end())
    return;
  if (inflight->second > 1) {
    --inflight->second;
  } else {
    state.mach_inflight_send_rights.erase(inflight);
  }
  static_cast<void>(enqueue_no_senders_notification_locked(state, object));
}

void discard_mach_message_rights_locked(
    KernelSharedState &state, const KernelSharedState::MachMessage &message) {
  const auto discard = [&](std::uint32_t object, xnu792::ipc::Right right) {
    switch (right) {
    case xnu792::ipc::Right::Send:
      release_inflight_send_right_locked(state, object);
      break;
    case xnu792::ipc::Right::Receive:
      terminate_receive_object_locked(state, object);
      break;
    case xnu792::ipc::Right::SendOnce:
      enqueue_send_once_notification_locked(state, object);
      break;
    case xnu792::ipc::Right::PortSet:
    case xnu792::ipc::Right::DeadName:
      break;
    }
  };
  if (message.reply_object && message.reply_right) {
    discard(*message.reply_object, *message.reply_right);
  }
  for (const auto &transfer : message.port_transfers) {
    discard(transfer.object, transfer.right);
  }
}

bool destroy_port_name_locked(KernelSharedState &state, std::uint32_t task,
                              std::uint32_t name) {
  const auto entry = state.mach_namespaces.lookup(task, name);
  if (!entry)
    return false;

  cancel_dead_name_notification_locked(state, task, name);
  if (!state.mach_namespaces.destroy_name(task, name))
    return false;

  const auto has = [&](xnu792::ipc::Right right) {
    return (entry->type & xnu792::ipc::type_mask(right)) != 0;
  };
  if (has(xnu792::ipc::Right::Send)) {
    static_cast<void>(
        enqueue_no_senders_notification_locked(state, entry->object));
  }
  if (has(xnu792::ipc::Right::SendOnce)) {
    enqueue_send_once_notification_locked(state, entry->object);
  }
  if (has(xnu792::ipc::Right::Receive)) {
    terminate_receive_object_locked(state, entry->object);
  } else if (has(xnu792::ipc::Right::PortSet)) {
    remove_port_object_locked(state, entry->object);
  }
  return true;
}

std::uint32_t modify_port_references_locked(KernelSharedState &state,
                                            std::uint32_t task,
                                            std::uint32_t name,
                                            xnu792::ipc::Right right,
                                            std::int32_t delta) {
  constexpr std::uint32_t kern_success = 0;
  constexpr std::uint32_t kern_invalid_name = 15;
  constexpr std::uint32_t kern_invalid_right = 17;
  constexpr std::uint32_t kern_invalid_value = 18;
  constexpr std::uint32_t kern_urefs_overflow = 19;

  if (name == xnu792::ipc::null_name || name == xnu792::ipc::dead_name) {
    return right == xnu792::ipc::Right::Send ||
                   right == xnu792::ipc::Right::SendOnce
               ? kern_success
               : kern_invalid_name;
  }
  const auto entry = state.mach_namespaces.lookup(task, name);
  if (!entry)
    return kern_invalid_name;
  const auto mask = xnu792::ipc::type_mask(right);
  if ((entry->type & mask) == 0)
    return kern_invalid_right;
  const auto references =
      state.mach_namespaces.user_references(task, name, right).value_or(0);

  if (right == xnu792::ipc::Right::Receive ||
      right == xnu792::ipc::Right::PortSet) {
    if (delta == 0)
      return kern_success;
    if (delta != -1)
      return kern_invalid_value;
    const auto remaining_type = entry->type & ~mask;
    if (right == xnu792::ipc::Right::Receive && remaining_type == 0) {
      cancel_dead_name_notification_locked(state, task, name);
    }
    static_cast<void>(state.mach_namespaces.remove_type(task, name, mask));
    if (right == xnu792::ipc::Right::Receive) {
      terminate_receive_object_locked(state, entry->object);
    } else {
      remove_port_object_locked(state, entry->object);
    }
    return kern_success;
  }

  if (right == xnu792::ipc::Right::SendOnce) {
    if (delta == 0)
      return kern_success;
    if (delta != -1)
      return kern_invalid_value;
    static_cast<void>(state.mach_namespaces.remove_type(task, name, mask));
    // A receive name may temporarily be composite with a send-once right when
    // it is also used as a notification endpoint.  Releasing only that
    // send-once right does not delete the name, so its dead-name request must
    // remain registered.
    if (!state.mach_namespaces.contains(task, name)) {
      cancel_dead_name_notification_locked(state, task, name);
    }
    enqueue_send_once_notification_locked(state, entry->object);
    return kern_success;
  }

  const auto updated = static_cast<std::int64_t>(references) + delta;
  if (updated < 0)
    return kern_invalid_value;
  const auto maximum = right == xnu792::ipc::Right::Send
                           ? xnu792::ipc::maximum_send_user_references
                           : xnu792::ipc::maximum_user_references;
  if (updated > maximum)
    return kern_urefs_overflow;
  if (!state.mach_namespaces.modify_references(task, name, right, delta)) {
    return kern_invalid_value;
  }
  if (updated == 0 && !state.mach_namespaces.contains(task, name)) {
    cancel_dead_name_notification_locked(state, task, name);
  }
  if (right == xnu792::ipc::Right::Send && updated == 0) {
    static_cast<void>(
        enqueue_no_senders_notification_locked(state, entry->object));
  }
  return kern_success;
}

} // namespace mach_support

} // namespace ilemu
