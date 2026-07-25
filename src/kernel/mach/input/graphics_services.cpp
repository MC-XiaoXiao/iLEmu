#include "ilegacysim/graphics_services_input.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <mutex>
#include <vector>

#include "ilegacysim/display.hpp"
#include "ilegacysim/mig_wire_abi.hpp"
#include "ilegacysim/scene_coordinator.hpp"
#include "ilegacysim/userland_hle.hpp"

namespace ilegacysim::graphics_services_input {
namespace {

constexpr std::uint32_t copy_send_bits = 19;
constexpr std::uint32_t graphics_event_message_id = 123;
constexpr std::uint32_t application_did_become_active_event_type = 50;
constexpr std::uint32_t hand_event_type = 3001;
constexpr std::uint32_t menu_button_down_event_type = 1000;
constexpr std::uint32_t menu_button_up_event_type = 1001;
constexpr std::uint32_t volume_up_button_down_event_type = 1006;
constexpr std::uint32_t volume_up_button_up_event_type = 1007;
constexpr std::uint32_t volume_down_button_down_event_type = 1008;
constexpr std::uint32_t volume_down_button_up_event_type = 1009;
constexpr std::uint32_t lock_button_down_event_type = 1010;
constexpr std::uint32_t lock_button_up_event_type = 1011;
constexpr std::uint32_t ringer_switch_off_event_type = 1012;
constexpr std::uint32_t ringer_switch_on_event_type = 1013;
constexpr std::size_t event_record_size = 48;
constexpr std::size_t hand_info_size = 20;
constexpr std::size_t path_info_size = 16;
constexpr std::size_t event_payload_size =
    event_record_size + hand_info_size + path_info_size;
constexpr std::size_t hand_message_size =
    darwin::mig_wire::message_header_size + event_payload_size;
constexpr std::size_t simple_event_message_size =
    darwin::mig_wire::message_header_size + event_record_size;

constexpr std::size_t record_location_offset = 8;
constexpr std::size_t record_window_location_offset = 16;
constexpr std::size_t record_timestamp_offset = 24;
constexpr std::size_t record_info_size_offset = 44;
constexpr std::size_t hand_offset =
    darwin::mig_wire::message_header_size + event_record_size;
constexpr std::size_t hand_path_count_offset = hand_offset + 17;
constexpr std::size_t path_offset = hand_offset + hand_info_size;
constexpr std::size_t path_pressure_offset = path_offset + 4;
constexpr std::size_t path_location_offset = path_offset + 8;

constexpr std::uint8_t path_index = 1;
constexpr std::uint8_t path_identity = 2;
constexpr std::uint8_t path_active_proximity = 3;

constexpr std::string_view springboard_image{
    "/System/Library/CoreServices/SpringBoard.app/SpringBoard"};

void write_word(std::span<std::byte> bytes, std::size_t offset,
                std::uint32_t value) {
  for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
    bytes[offset + byte] = static_cast<std::byte>(value >> (byte * 8U));
  }
}

void write_float(std::span<std::byte> bytes, std::size_t offset, float value) {
  write_word(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

std::uint32_t read_word(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
    value |= std::to_integer<std::uint32_t>(bytes[offset + byte])
             << (byte * 8U);
  }
  return value;
}

std::uint32_t hand_type(TouchPhase phase) {
  // These are the values consumed by this firmware's UIKit binary, not the
  // values published by reconstructed headers for later iPhone OS releases.
  // UIWindow::sendEvent: branches on 1 (down), 2 (drag), and 5 (up). In
  // particular, 0 reaches the correct window but is ignored before _mouseDown:.
  switch (phase) {
  case TouchPhase::Down:
    return 1;
  case TouchPhase::Move:
    return 2;
  case TouchPhase::Up:
    return 5;
  case TouchPhase::Cancel:
    return 3;
  }
  return 3;
}

bool active(TouchPhase phase) {
  return phase == TouchPhase::Down || phase == TouchPhase::Move;
}

std::uint32_t system_button_event_type(const SystemButtonInput &input) {
  const auto down = input.phase == SystemButtonPhase::Down;
  switch (input.button) {
  case SystemButton::Home:
    return down ? menu_button_down_event_type : menu_button_up_event_type;
  case SystemButton::Lock:
    return down ? lock_button_down_event_type : lock_button_up_event_type;
  case SystemButton::VolumeUp:
    return down ? volume_up_button_down_event_type
                : volume_up_button_up_event_type;
  case SystemButton::VolumeDown:
    return down ? volume_down_button_down_event_type
                : volume_down_button_up_event_type;
  }
  return menu_button_up_event_type;
}

KernelSharedState::MachMessage make_touch_message(std::uint32_t destination,
                                                  std::uint64_t timestamp,
                                                  const TouchInput &input) {
  KernelSharedState::MachMessage message;
  message.bytes.resize(hand_message_size, std::byte{0});
  message.destination = destination;
  message.sender_pid = 0;

  write_word(message.bytes, darwin::mig_wire::header_bits_offset,
             copy_send_bits);
  write_word(message.bytes, darwin::mig_wire::header_size_offset,
             static_cast<std::uint32_t>(hand_message_size));
  write_word(message.bytes, darwin::mig_wire::header_remote_port_offset,
             destination);
  write_word(message.bytes, darwin::mig_wire::header_identifier_offset,
             graphics_event_message_id);

  const auto record = darwin::mig_wire::message_header_size;
  write_word(message.bytes, record, hand_event_type);
  write_float(message.bytes, record + record_location_offset, input.x);
  write_float(message.bytes, record + record_location_offset + 4, input.y);
  write_float(message.bytes, record + record_window_location_offset, input.x);
  write_float(message.bytes, record + record_window_location_offset + 4,
              input.y);
  write_word(message.bytes, record + record_timestamp_offset,
             static_cast<std::uint32_t>(timestamp));
  write_word(message.bytes, record + record_timestamp_offset + 4,
             static_cast<std::uint32_t>(timestamp >> 32U));
  write_word(message.bytes, record + record_info_size_offset,
             static_cast<std::uint32_t>(hand_info_size + path_info_size));

  write_word(message.bytes, hand_offset, hand_type(input.phase));
  message.bytes[hand_path_count_offset] = std::byte{1};
  message.bytes[path_offset] = static_cast<std::byte>(path_index);
  message.bytes[path_offset + 1] = static_cast<std::byte>(path_identity);
  message.bytes[path_offset + 2] =
      static_cast<std::byte>(active(input.phase) ? path_active_proximity : 0);
  write_float(message.bytes, path_pressure_offset,
              active(input.phase) ? 1.0F : 0.0F);
  write_float(message.bytes, path_location_offset, input.x);
  write_float(message.bytes, path_location_offset + 4, input.y);
  return message;
}

KernelSharedState::MachMessage
make_simple_event_message(std::uint32_t destination, std::uint64_t timestamp,
                          std::uint32_t event_type) {
  KernelSharedState::MachMessage message;
  message.bytes.resize(simple_event_message_size, std::byte{0});
  message.destination = destination;
  message.sender_pid = 0;

  write_word(message.bytes, darwin::mig_wire::header_bits_offset,
             copy_send_bits);
  write_word(message.bytes, darwin::mig_wire::header_size_offset,
             static_cast<std::uint32_t>(simple_event_message_size));
  write_word(message.bytes, darwin::mig_wire::header_remote_port_offset,
             destination);
  write_word(message.bytes, darwin::mig_wire::header_identifier_offset,
             graphics_event_message_id);
  const auto record = darwin::mig_wire::message_header_size;
  write_word(message.bytes, record, event_type);
  write_word(message.bytes, record + record_timestamp_offset,
             static_cast<std::uint32_t>(timestamp));
  write_word(message.bytes, record + record_timestamp_offset + 4,
             static_cast<std::uint32_t>(timestamp >> 32U));
  return message;
}

void queue_locked(KernelSharedState &state, std::uint32_t destination,
                  const TouchInput &input) {
  state.mach_queues[destination].push_back(
      make_touch_message(destination, state.clock.now(), input));
}

void queue_simple_event_locked(KernelSharedState &state,
                               std::uint32_t destination,
                               std::uint32_t event_type) {
  state.mach_queues[destination].push_back(
      make_simple_event_message(destination, state.clock.now(), event_type));
}

} // namespace

void register_springboard_alert_observers(
    UserlandHleRegistry &registry,
    std::function<void(std::uint32_t, bool)> observer) {
  registry.register_objc_instance_method(
      std::string{springboard_image}, "SBAlertItemsController",
      "activateAlertItem:",
      "-[SBAlertItemsController activateAlertItem:]",
      [observer](UserlandHleCall &call) {
        const auto object = call.argument(2);
        observer(object, true);
        call.resume_original_persistently();
      });
  registry.register_objc_instance_method(
      std::string{springboard_image}, "SBAlertItemsController",
      "deactivateAlertItem:",
      "-[SBAlertItemsController deactivateAlertItem:]",
      [observer = std::move(observer)](UserlandHleCall &call) {
        const auto object = call.argument(2);
        observer(object, false);
        call.resume_original_persistently();
      });
}

std::optional<std::uint32_t> event_type(std::span<const std::byte> message) {
  constexpr std::size_t event_type_offset =
      darwin::mig_wire::message_header_size;
  if (message.size() < event_type_offset + sizeof(std::uint32_t) ||
      read_word(message, darwin::mig_wire::header_identifier_offset) !=
          graphics_event_message_id) {
    return std::nullopt;
  }
  return read_word(message, event_type_offset);
}

void record_bootstrap_lookup_locked(KernelSharedState &state,
                                    std::uint32_t reply_object,
                                    std::string_view service_name) {
  if (reply_object != 0 && !service_name.empty()) {
    state.pending_bootstrap_service_lookups[reply_object] =
        std::string{service_name};
  }
}

void record_bootstrap_registration_locked(KernelSharedState &state,
                                          std::string_view service_name) {
  if (service_name.empty())
    return;
  auto &generation =
      state.bootstrap_service_generations[std::string{service_name}];
  ++generation;
  if (generation == 0U)
    generation = 1U;
}

ServiceResolution record_bootstrap_reply_locked(
    KernelSharedState &state, std::uint32_t reply_object,
    std::span<const KernelSharedState::MachMessage::PortTransfer> transfers,
    std::uint32_t receiver_process_id) {
  const auto pending =
      state.pending_bootstrap_service_lookups.find(reply_object);
  if (pending == state.pending_bootstrap_service_lookups.end())
    return {};

  const auto service_name = std::move(pending->second);
  state.pending_bootstrap_service_lookups.erase(pending);
  const auto service = std::find_if(
      transfers.begin(), transfers.end(), [](const auto &transfer) {
        return transfer.right == xnu792::ipc::Right::Send;
      });
  const auto reply_port = state.mach_port_objects.lookup(reply_object);
  const auto receiver = reply_port && reply_port->receive_owner != 0U
                            ? reply_port->receive_owner
                            : receiver_process_id;
  if (service == transfers.end()) {
    if (receiver != 0U) {
      const auto generation =
          state.bootstrap_service_generations[service_name];
      state.pending_bootstrap_retries[receiver] =
          PendingTimer::BootstrapRetry{service_name, generation};
    }
    return ServiceResolution{0, 0, false, service_name};
  }
  if (receiver != 0U) {
    const auto retry = state.pending_bootstrap_retries.find(receiver);
    if (retry != state.pending_bootstrap_retries.end() &&
        retry->second.service_name == service_name) {
      state.pending_bootstrap_retries.erase(retry);
    }
  }

  std::size_t flushed = 0;
  bool application_event_port = false;
  if (service_name == system_event_service) {
    state.bootstrap_service_objects[service_name] = service->object;
    while (!state.pending_graphics_inputs.empty()) {
      const auto &input = state.pending_graphics_inputs.front();
      if (input.kind == KernelSharedState::PendingGraphicsInput::Kind::Touch) {
        queue_locked(state, service->object, input.touch);
      } else {
        queue_simple_event_locked(state, service->object,
                                  input.system_event_type);
      }
      state.pending_graphics_inputs.pop_front();
      ++flushed;
    }
  } else if (const auto port =
                 state.mach_port_objects.lookup(service->object)) {
    const auto process = state.processes.find(port->receive_owner);
    if (process != state.processes.end() && !process->second.exited &&
        process->second.executable_path.starts_with("/Applications/")) {
      if (state.pending_application_event_object != service->object &&
          state.latest_application_scene_transform &&
          state.latest_application_scene_transform->process_id ==
              port->receive_owner) {
        state.latest_application_scene_transform.reset();
      }
      state.pending_application_event_object = service->object;
      application_event_port = true;
    }
  }
  return ServiceResolution{service->object, flushed, application_event_port,
                           service_name};
}

EnqueueResult enqueue_touch(KernelSharedState &state, const TouchInput &input,
                            const SceneCoordinator *scenes) {
  const TouchInput sanitized{input.phase,
                             std::isfinite(input.x) ? input.x : 0.0F,
                             std::isfinite(input.y) ? input.y : 0.0F};
  std::lock_guard lock{state.mach_mutex};
  if (state.active_springboard_alert_items.empty() &&
      state.active_application_event_object != 0U &&
      !state.application_touch_suspended) {
    const auto port =
        state.mach_port_objects.lookup(state.active_application_event_object);
    const auto process = port ? state.processes.find(port->receive_owner)
                              : state.processes.end();
    if (port && process != state.processes.end() && !process->second.exited &&
        process->second.executable_path.starts_with("/Applications/")) {
      auto application_input = sanitized;
      if (scenes) {
        const auto scene = scenes->client_scene(port->receive_owner);
        if (scene && scene->state == ClientSceneState::Active) {
          if (scene->input_transform) {
            const auto [x, y] =
                scene->input_transform->map(sanitized.x, sanitized.y);
            application_input.x = x;
            application_input.y = y;
          }
          queue_locked(state, state.active_application_event_object,
                       application_input);
          return EnqueueResult::Queued;
        }
        // Preserve the Mach route while the semantic client is suspended or
        // only committed, but keep the host event with SpringBoard.
      } else {
        if (state.active_application_scene &&
            state.active_application_scene->process_id ==
                port->receive_owner &&
            state.active_application_scene->event_object ==
                state.active_application_event_object &&
            state.active_application_scene->touch_transform) {
          const auto &transform =
              *state.active_application_scene->touch_transform;
          application_input.x -= transform.presentation_offset_x;
          application_input.y -= transform.presentation_offset_y;
        }
        queue_locked(state, state.active_application_event_object,
                     application_input);
        return EnqueueResult::Queued;
      }
    } else {
      state.active_application_event_object = 0U;
      state.application_touch_suspended = false;
    }
  }
  const auto service =
      state.bootstrap_service_objects.find(std::string{system_event_service});
  if (service == state.bootstrap_service_objects.end()) {
    state.pending_graphics_inputs.push_back(
        KernelSharedState::PendingGraphicsInput{
            KernelSharedState::PendingGraphicsInput::Kind::Touch, sanitized,
            0});
    return EnqueueResult::Deferred;
  }
  queue_locked(state, service->second, sanitized);
  return EnqueueResult::Queued;
}

EnqueueResult enqueue_system_button(KernelSharedState &state,
                                    const SystemButtonInput &input) {
  const auto event_type = system_button_event_type(input);
  std::lock_guard lock{state.mach_mutex};
  const auto service =
      state.bootstrap_service_objects.find(std::string{system_event_service});
  if (service == state.bootstrap_service_objects.end()) {
    state.pending_graphics_inputs.push_back(
        KernelSharedState::PendingGraphicsInput{
            KernelSharedState::PendingGraphicsInput::Kind::SystemEvent,
            {},
            event_type});
    return EnqueueResult::Deferred;
  }
  queue_simple_event_locked(state, service->second, event_type);
  return EnqueueResult::Queued;
}

EnqueueResult enqueue_ringer_switch_change(KernelSharedState &state,
                                           bool active) {
  const auto event_type =
      active ? ringer_switch_on_event_type : ringer_switch_off_event_type;
  std::lock_guard lock{state.mach_mutex};
  const auto service =
      state.bootstrap_service_objects.find(std::string{system_event_service});
  if (service == state.bootstrap_service_objects.end()) {
    state.pending_graphics_inputs.push_back(
            KernelSharedState::PendingGraphicsInput{
                KernelSharedState::PendingGraphicsInput::Kind::SystemEvent,
                {},
                event_type});
    return EnqueueResult::Deferred;
  }
  queue_simple_event_locked(state, service->second, event_type);
  return EnqueueResult::Queued;
}

void suspend_active_application(
    KernelSharedState &state,
    KernelSharedState::ApplicationSuspensionReason reason,
    SceneCoordinator *scenes) {
  std::lock_guard lock{state.mach_mutex};
  state.application_touch_suspended =
      state.active_application_event_object != 0U;
  state.application_suspension_reason =
      state.application_touch_suspended
          ? reason
          : KernelSharedState::ApplicationSuspensionReason::None;
  if (!state.application_touch_suspended) {
    state.suspended_application_scene_process_id.reset();
    return;
  }
  if (state.active_application_scene) {
    state.suspended_application_scene_process_id =
        state.active_application_scene->process_id;
    if (scenes) {
      scenes->suspend_client_scene(
          state.active_application_scene->process_id);
    }
    return;
  }
  if (const auto active_port = state.mach_port_objects.lookup(
          state.active_application_event_object)) {
    state.suspended_application_scene_process_id = active_port->receive_owner;
    if (scenes)
      scenes->suspend_client_scene(active_port->receive_owner);
  }
}

void activate_resolved_application(KernelSharedState &state,
                                   std::uint32_t process_id,
                                   SceneCoordinator *scenes) {
  std::lock_guard lock{state.mach_mutex};
  const auto scene_committed =
      scenes ? scenes->client_scene(process_id).has_value()
             : state.active_application_scene &&
                   state.active_application_scene->touch_transform.has_value();
  const auto owns_active_intent =
      state.active_application_scene &&
      state.active_application_scene->process_id == process_id;
  auto event_object =
      owns_active_intent ? state.active_application_scene->event_object : 0U;
  if (const auto pending_port = state.mach_port_objects.lookup(
          state.pending_application_event_object);
      pending_port && pending_port->receive_owner == process_id) {
    event_object = state.pending_application_event_object;
  }
  const auto process = state.processes.find(process_id);
  const auto valid_application =
      process != state.processes.end() && !process->second.exited &&
      process->second.executable_path.starts_with("/Applications/");
  const auto requests_userspace_prewarm =
      valid_application &&
      std::find(process->second.arguments.begin(),
                process->second.arguments.end(),
                "--suspended") != process->second.arguments.end();
  const auto preserves_committed_foreground =
      state.active_application_scene && !owns_active_intent &&
      state.active_application_event_object ==
          state.active_application_scene->event_object &&
      !state.application_touch_suspended &&
      (scenes ? scenes->client_scene_active(
                    state.active_application_scene->process_id)
              : state.active_application_scene->touch_transform.has_value());
  if (scene_committed && event_object != 0U && valid_application &&
      !preserves_committed_foreground &&
      (!requests_userspace_prewarm || owns_active_intent)) {
    std::optional<KernelSharedState::ApplicationTouchTransform> transform;
    if (const auto cached =
            state.application_scene_transforms.find(process_id);
        cached != state.application_scene_transforms.end()) {
      transform = cached->second;
    } else if (owns_active_intent) {
      transform = state.active_application_scene->touch_transform;
    }
    state.active_application_scene = KernelSharedState::ActiveApplicationScene{
        process_id, event_object, transform};
    state.active_application_event_object = event_object;
    state.application_touch_suspended = false;
    state.application_suspension_reason =
        KernelSharedState::ApplicationSuspensionReason::None;
    state.suspended_application_scene_process_id.reset();
    if (scenes)
      scenes->activate_client_scene(process_id);
  }
}

void reset_application_scene_context(KernelSharedState &state,
                                     std::uint32_t render_process_id,
                                     std::uint32_t context) {
  std::lock_guard lock{state.mach_mutex};
  state.application_scene_context_owners.erase(
      std::pair{render_process_id, context});
}

std::optional<std::uint32_t> record_application_scene_transform(
    KernelSharedState &state, std::uint32_t render_process_id,
    std::uint32_t context,
    const KernelSharedState::ApplicationTouchTransform &transform) {
  if (!std::isfinite(transform.presentation_offset_x) ||
      !std::isfinite(transform.presentation_offset_y) ||
      !std::isfinite(transform.screen_origin_y)) {
    return std::nullopt;
  }
  std::lock_guard lock{state.mach_mutex};
  const auto context_key = std::pair{render_process_id, context};
  const auto owner = state.application_scene_context_owners.find(context_key);
  std::uint32_t process_id{};
  if (owner != state.application_scene_context_owners.end()) {
    process_id = owner->second;
  } else {
    const auto pending_port =
        state.mach_port_objects.lookup(state.pending_application_event_object);
    if (!pending_port) {
      return std::nullopt;
    }
    process_id = pending_port->receive_owner;
  }
  // SpringBoard publishes separate full-screen roots for the outgoing App's
  // lock screen and exit snapshots. Ignore only that App's roots: a different
  // App can publish its first live root before didBecomeActive arrives.
  if (state.application_touch_suspended &&
      state.suspended_application_scene_process_id == process_id) {
    return std::nullopt;
  }
  const auto process = state.processes.find(process_id);
  if (process == state.processes.end() || process->second.exited ||
      !process->second.executable_path.starts_with("/Applications/")) {
    return std::nullopt;
  }
  if (owner == state.application_scene_context_owners.end()) {
    state.application_scene_context_owners.emplace(context_key, process_id);
  }
  const auto owns_active_route =
      state.active_application_scene &&
      state.active_application_scene->process_id == process_id;
  state.latest_application_scene_transform =
      KernelSharedState::PendingApplicationSceneTransform{process_id,
                                                          transform};
  state.application_scene_transforms[process_id] = transform;
  if (owns_active_route) {
    state.active_application_scene->touch_transform = transform;
    state.active_application_event_object =
        state.active_application_scene->event_object;
  }
  return process_id;
}

void release_application_process_locked(KernelSharedState &state,
                                        std::uint32_t process_id) {
  const auto object_owned_by_process = [&state, process_id](
                                           std::uint32_t object) {
    const auto port = state.mach_port_objects.lookup(object);
    return port && port->receive_owner == process_id;
  };
  const auto active_scene_owned =
      state.active_application_scene &&
      state.active_application_scene->process_id == process_id;
  const auto suspended_scene_owned =
      state.suspended_application_scene_process_id == process_id;
  const auto active_event = state.active_application_event_object;
  const auto active_route_owned =
      active_scene_owned || suspended_scene_owned ||
      object_owned_by_process(active_event);

  if (object_owned_by_process(state.pending_application_event_object) ||
      (active_route_owned &&
       state.pending_application_event_object == active_event)) {
    state.pending_application_event_object = 0;
  }
  if (active_route_owned) {
    state.active_application_event_object = 0;
  }
  if (active_route_owned || suspended_scene_owned) {
    state.application_touch_suspended = false;
    state.application_suspension_reason =
        KernelSharedState::ApplicationSuspensionReason::None;
  }
  if (suspended_scene_owned) {
    state.suspended_application_scene_process_id.reset();
  }
  if (state.latest_application_scene_transform &&
      state.latest_application_scene_transform->process_id == process_id) {
    state.latest_application_scene_transform.reset();
  }
  if (active_scene_owned) {
    state.active_application_scene.reset();
  }
  if (state.pending_application_exit_snapshot &&
      state.pending_application_exit_snapshot->process_id == process_id) {
    state.pending_application_exit_snapshot.reset();
  }
  state.application_scene_transforms.erase(process_id);
  state.consumed_application_prewarm_activations.erase(process_id);
  std::erase_if(state.application_scene_context_owners,
                [process_id](const auto &owner) {
                  return owner.second == process_id;
                });
}

void record_application_lifecycle_event_locked(
    KernelSharedState &state, std::uint32_t sender_pid,
    std::uint32_t destination, std::uint32_t event_type,
    std::span<const std::uint32_t> exit_snapshot_pixels,
    SceneCoordinator *scenes) {
  if (event_type != application_did_become_active_event_type &&
      event_type != application_will_resign_active_event_type) {
    return;
  }
  const auto sender = state.processes.find(sender_pid);
  if (sender == state.processes.end() || sender->second.exited ||
      !sender->second.executable_path.ends_with(
          "/SpringBoard.app/SpringBoard")) {
    return;
  }
  const auto destination_port = state.mach_port_objects.lookup(destination);
  const auto application =
      destination_port ? state.processes.find(destination_port->receive_owner)
                       : state.processes.end();
  if (!destination_port || application == state.processes.end() ||
      application->second.exited ||
      !application->second.executable_path.starts_with("/Applications/")) {
    return;
  }

  if (event_type == application_did_become_active_event_type) {
    state.pending_application_event_object = destination;
    const auto requests_userspace_prewarm =
        std::find(application->second.arguments.begin(),
                  application->second.arguments.end(), "--suspended") !=
        application->second.arguments.end();
    if (requests_userspace_prewarm &&
        state.consumed_application_prewarm_activations
            .insert(destination_port->receive_owner)
            .second) {
      return;
    }
    std::optional<KernelSharedState::ApplicationTouchTransform> transform;
    if (state.latest_application_scene_transform &&
        state.latest_application_scene_transform->process_id ==
            destination_port->receive_owner) {
      transform = state.latest_application_scene_transform->transform;
    } else if (state.active_application_scene &&
               state.active_application_scene->process_id ==
                   destination_port->receive_owner &&
               state.active_application_scene->event_object == destination) {
      transform = state.active_application_scene->touch_transform;
    } else if (const auto cached = state.application_scene_transforms.find(
                   destination_port->receive_owner);
               cached != state.application_scene_transforms.end()) {
      transform = cached->second;
    }
    const auto semantic_scene_committed =
        scenes &&
        scenes->client_scene(destination_port->receive_owner).has_value();
    // A suspended/event-only process can receive the same activation event as
    // a foreground application while another committed scene remains front.
    // Treat lifecycle delivery as intent, not proof of visibility: the
    // existing foreground route remains authoritative until it resigns or the
    // replacement has become the only committed scene.
    const auto preserves_committed_foreground =
        state.active_application_scene &&
        state.active_application_scene->process_id !=
            destination_port->receive_owner &&
        (scenes ? scenes->client_scene_active(
                       state.active_application_scene->process_id)
                : state.active_application_scene->touch_transform.has_value()) &&
        state.active_application_event_object ==
            state.active_application_scene->event_object &&
        !state.application_touch_suspended;
    if (preserves_committed_foreground) {
      if (state.latest_application_scene_transform &&
          state.latest_application_scene_transform->process_id ==
              destination_port->receive_owner) {
        state.latest_application_scene_transform.reset();
      }
      return;
    }
    state.active_application_scene = KernelSharedState::ActiveApplicationScene{
        destination_port->receive_owner, destination, transform};
    if (scenes ? semantic_scene_committed : transform.has_value()) {
      state.active_application_event_object = destination;
      state.application_touch_suspended = false;
      if (scenes)
        scenes->activate_client_scene(destination_port->receive_owner);
    } else {
      state.active_application_event_object = 0;
      state.application_touch_suspended = false;
    }
    state.application_suspension_reason =
        KernelSharedState::ApplicationSuspensionReason::None;
    state.suspended_application_scene_process_id.reset();
    if (state.latest_application_scene_transform &&
        state.latest_application_scene_transform->process_id ==
            destination_port->receive_owner) {
      state.latest_application_scene_transform.reset();
    }
    if (state.pending_application_exit_snapshot &&
        state.pending_application_exit_snapshot->process_id ==
            destination_port->receive_owner) {
      state.pending_application_exit_snapshot.reset();
    }
  } else {
    if (scenes)
      scenes->suspend_client_scene(destination_port->receive_owner);
    state.application_touch_suspended = true;
    state.suspended_application_scene_process_id =
        destination_port->receive_owner;
    if (!exit_snapshot_pixels.empty()) {
      std::vector<std::uint32_t> application_pixels{
          exit_snapshot_pixels.begin(), exit_snapshot_pixels.end()};
      auto screen_origin_y = 0.0F;
      if (state.active_application_scene &&
          state.active_application_scene->process_id ==
              destination_port->receive_owner &&
          state.active_application_scene->touch_transform) {
        screen_origin_y =
            state.active_application_scene->touch_transform->screen_origin_y;
      } else if (const auto cached = state.application_scene_transforms.find(
                     destination_port->receive_owner);
                 cached != state.application_scene_transforms.end()) {
        screen_origin_y = cached->second.screen_origin_y;
      }
      const auto geometry = state.display_geometry;
      const auto ui_geometry = state.user_interface_geometry;
      const auto scaled_origin_y =
          ui_geometry.height == 0U
              ? screen_origin_y
              : screen_origin_y * static_cast<float>(geometry.height) /
                    static_cast<float>(ui_geometry.height);
      const auto rounded_origin_y = std::llround(scaled_origin_y);
      if (rounded_origin_y > 0 && rounded_origin_y < geometry.height &&
          application_pixels.size() ==
              geometry.pixel_count()) {
        const auto inset = static_cast<std::size_t>(rounded_origin_y);
        const auto client_height =
            static_cast<std::size_t>(geometry.height) - inset;
        std::vector<std::uint32_t> local_pixels(application_pixels.size());
        for (std::size_t target_y = 0; target_y < geometry.height;
             ++target_y) {
          const auto client_y =
              std::min(client_height - 1U,
                       target_y * client_height / geometry.height);
          const auto source = (inset + client_y) * geometry.width;
          const auto destination_offset = target_y * geometry.width;
          std::copy_n(application_pixels.begin() +
                          static_cast<std::ptrdiff_t>(source),
                      geometry.width,
                      local_pixels.begin() +
                          static_cast<std::ptrdiff_t>(destination_offset));
        }
        application_pixels = std::move(local_pixels);
      }
      state.pending_application_exit_snapshot =
          KernelSharedState::ApplicationExitSnapshot{
              destination_port->receive_owner, std::move(application_pixels)};
    }
    const auto preserve_locked_scene =
        state.application_suspension_reason ==
            KernelSharedState::ApplicationSuspensionReason::Lock &&
        state.active_application_scene &&
        state.active_application_scene->process_id ==
            destination_port->receive_owner;
    state.application_suspension_reason =
        KernelSharedState::ApplicationSuspensionReason::None;
    if (preserve_locked_scene) {
      return;
    }
    if (state.latest_application_scene_transform &&
        state.latest_application_scene_transform->process_id ==
            destination_port->receive_owner) {
      state.latest_application_scene_transform.reset();
    }
    for (auto context = state.application_scene_context_owners.begin();
         context != state.application_scene_context_owners.end();) {
      if (context->second == destination_port->receive_owner) {
        context = state.application_scene_context_owners.erase(context);
      } else {
        ++context;
      }
    }
    state.active_application_scene.reset();
  }
}

} // namespace ilegacysim::graphics_services_input
