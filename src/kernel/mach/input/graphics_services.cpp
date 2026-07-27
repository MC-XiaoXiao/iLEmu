#include "ilemu/graphics_services_input.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <mutex>
#include <vector>

#include "ilemu/display.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/scene_coordinator.hpp"
#include "ilemu/userland_hle.hpp"

namespace ilemu::graphics_services_input {
namespace {

constexpr std::uint32_t copy_send_bits = 19;
constexpr std::uint32_t graphics_event_message_id = 123;
constexpr std::uint32_t application_did_become_active_event_type = 50;
constexpr std::uint32_t application_did_finish_background_event_type = 2003;
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

std::uint32_t mouse_event_type(TouchPhase phase) {
  switch (phase) {
  case TouchPhase::Down:
    return 1;
  case TouchPhase::Move:
    return 6;
  case TouchPhase::Up:
  case TouchPhase::Cancel:
    return 2;
  }
  return 2;
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

KernelSharedState::MachMessage::GraphicsInputKind
system_button_input_kind(SystemButton button) {
  switch (button) {
  case SystemButton::Home:
    return KernelSharedState::MachMessage::GraphicsInputKind::Home;
  case SystemButton::Lock:
    return KernelSharedState::MachMessage::GraphicsInputKind::Lock;
  case SystemButton::VolumeUp:
  case SystemButton::VolumeDown:
    return KernelSharedState::MachMessage::GraphicsInputKind::OtherSystem;
  }
  return KernelSharedState::MachMessage::GraphicsInputKind::OtherSystem;
}

std::uint64_t allocate_graphics_input_sequence_locked(
    KernelSharedState &state) {
  auto sequence = state.next_graphics_input_sequence++;
  if (sequence == 0U) {
    sequence = 1U;
    state.next_graphics_input_sequence = 2U;
  }
  return sequence;
}

void clear_springboard_enqueued_gesture_locked(KernelSharedState &state) {
  state.springboard_enqueued_active_touch_begin_sequence = 0U;
  state.springboard_enqueued_last_touch_begin_sequence = 0U;
  state.springboard_enqueued_last_touch_end_sequence = 0U;
  state.springboard_pending_launch_touch_sequence = 0U;
}

KernelSharedState::MachMessage make_touch_message(std::uint32_t destination,
                                                  std::uint64_t timestamp,
                                                  const TouchInput &input,
                                                  KernelSharedState::
                                                      GraphicsInputAbi abi,
                                                  std::uint64_t input_sequence) {
  KernelSharedState::MachMessage message;
  message.bytes.resize(hand_message_size, std::byte{0});
  message.destination = destination;
  message.sender_pid = 0;
  message.graphics_input_sequence = input_sequence;
  message.graphics_input_kind =
      KernelSharedState::MachMessage::GraphicsInputKind::Touch;
  message.graphics_touch_phase = input.phase;

  write_word(message.bytes, darwin::mig_wire::header_bits_offset,
             copy_send_bits);
  write_word(message.bytes, darwin::mig_wire::header_size_offset,
             static_cast<std::uint32_t>(hand_message_size));
  write_word(message.bytes, darwin::mig_wire::header_remote_port_offset,
             destination);
  write_word(message.bytes, darwin::mig_wire::header_identifier_offset,
             graphics_event_message_id);

  const auto record = darwin::mig_wire::message_header_size;
  write_word(message.bytes, record,
             abi == KernelSharedState::GraphicsInputAbi::UIKitHand
                 ? hand_event_type
                 : mouse_event_type(input.phase));
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
                          std::uint32_t event_type,
                          std::uint64_t input_sequence,
                          KernelSharedState::MachMessage::GraphicsInputKind
                              input_kind) {
  KernelSharedState::MachMessage message;
  message.bytes.resize(simple_event_message_size, std::byte{0});
  message.destination = destination;
  message.sender_pid = 0;
  message.graphics_input_sequence = input_sequence;
  message.graphics_input_kind = input_kind;

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
                  const TouchInput &input, std::uint64_t input_sequence) {
  auto abi = KernelSharedState::GraphicsInputAbi::UIKitHand;
  if (const auto port = state.mach_port_objects.lookup(destination)) {
    if (const auto process = state.processes.find(port->receive_owner);
        process != state.processes.end()) {
      abi = process->second.graphics_input_abi;
    }
  }
  state.mach_queues[destination].push_back(
      make_touch_message(destination, state.clock.now(), input, abi,
                         input_sequence));
}

void queue_simple_event_locked(KernelSharedState &state,
                               std::uint32_t destination,
                               std::uint32_t event_type,
                               std::uint64_t input_sequence,
                               KernelSharedState::MachMessage::GraphicsInputKind
                                   input_kind) {
  state.mach_queues[destination].push_back(make_simple_event_message(
      destination, state.clock.now(), event_type, input_sequence, input_kind));
}

bool object_owned_by_process_locked(const KernelSharedState &state,
                                    std::uint32_t object,
                                    std::uint32_t process_id) {
  const auto port = state.mach_port_objects.lookup(object);
  return port && port->receive_owner == process_id;
}

bool process_is_springboard_locked(const KernelSharedState &state,
                                   std::uint32_t process_id) {
  const auto process = state.processes.find(process_id);
  return process != state.processes.end() && !process->second.exited &&
         process->second.executable_path.ends_with(
             "/SpringBoard.app/SpringBoard");
}

std::uint64_t allocate_application_launch_token_locked(
    KernelSharedState &state) {
  auto token = state.next_application_launch_token++;
  if (token == 0U) {
    token = 1U;
    state.next_application_launch_token = 2U;
  }
  return token;
}

std::uint64_t springboard_launch_origin_touch_sequence_locked(
    const KernelSharedState &state) {
  if (state.springboard_pending_launch_touch_sequence != 0U)
    return state.springboard_pending_launch_touch_sequence;
  if (state.foreground_application_attempt_process_id) {
    const auto attempt = state.application_launch_attempts.find(
        *state.foreground_application_attempt_process_id);
    if (attempt != state.application_launch_attempts.end() &&
        attempt->second.origin_touch_sequence != 0U) {
      return attempt->second.origin_touch_sequence;
    }
  }
  // Home can arrive after SpringBoard has selected and spawned an App but
  // before it asks launchd for that App's event service. The foreground
  // authorization is deliberately cleared at Home, so retain causality
  // through the exact outgoing PID instead of falling back to an unrelated
  // historical touch.
  if (state.application_touch_suspended &&
      state.application_suspension_reason ==
          KernelSharedState::ApplicationSuspensionReason::Home &&
      state.suspended_application_scene_process_id) {
    const auto attempt = state.application_launch_attempts.find(
        *state.suspended_application_scene_process_id);
    if (attempt != state.application_launch_attempts.end() &&
        attempt->second.origin_touch_sequence != 0U) {
      return attempt->second.origin_touch_sequence;
    }
  }
  return 0U;
}

std::optional<std::uint64_t>
pending_held_application_origin_locked(
    const KernelSharedState &state, std::uint32_t process_id = 0U) {
  if (!state.held_application_launch || !state.application_launch_barrier ||
      state.application_launch_barrier->reason !=
          KernelSharedState::ApplicationSuspensionReason::Lock) {
    return std::nullopt;
  }
  const auto &held = *state.held_application_launch;
  if (held.origin_touch_sequence == 0U ||
      held.origin_touch_sequence >= held.lock_input_sequence ||
      held.lock_input_sequence !=
          state.application_launch_barrier->input_sequence ||
      (state.last_home_launch_barrier_sequence != 0U &&
       held.origin_touch_sequence <
           state.last_home_launch_barrier_sequence) ||
      (process_id != 0U && held.process_id != 0U &&
       held.process_id != process_id)) {
    return std::nullopt;
  }
  return held.origin_touch_sequence;
}

KernelSharedState::ApplicationSuspensionReason interruption_reason(
    KernelSharedState::ApplicationLaunchPhase phase) {
  switch (phase) {
  case KernelSharedState::ApplicationLaunchPhase::InterruptedHome:
    return KernelSharedState::ApplicationSuspensionReason::Home;
  case KernelSharedState::ApplicationLaunchPhase::Launching:
  case KernelSharedState::ApplicationLaunchPhase::Active:
  case KernelSharedState::ApplicationLaunchPhase::Suspended:
  case KernelSharedState::ApplicationLaunchPhase::HeldLock:
    return KernelSharedState::ApplicationSuspensionReason::None;
  }
  return KernelSharedState::ApplicationSuspensionReason::None;
}

bool attempt_interrupted(
    const KernelSharedState::ApplicationLaunchAttempt &attempt) {
  return attempt.phase ==
         KernelSharedState::ApplicationLaunchPhase::InterruptedHome;
}

bool attempt_held_by_lock(
    const KernelSharedState::ApplicationLaunchAttempt &attempt) {
  return attempt.phase == KernelSharedState::ApplicationLaunchPhase::HeldLock;
}

bool origin_is_unlock_touch_locked(const KernelSharedState &state,
                                   std::uint64_t origin_touch_sequence) {
  if (state.springboard_unlock_touch_begin_sequence == 0U ||
      origin_touch_sequence <
          state.springboard_unlock_touch_begin_sequence) {
    return false;
  }
  if (state.springboard_unlock_touch_active)
    return true;
  return state.springboard_unlock_touch_end_sequence != 0U &&
         origin_touch_sequence <=
             state.springboard_unlock_touch_end_sequence;
}

KernelSharedState::ApplicationLaunchAttempt *launch_attempt_locked(
    KernelSharedState &state, std::uint32_t process_id) {
  const auto attempt = state.application_launch_attempts.find(process_id);
  return attempt == state.application_launch_attempts.end()
             ? nullptr
             : &attempt->second;
}

bool attempt_is_home_exit_target_locked(
    const KernelSharedState &state, std::uint32_t process_id,
    const KernelSharedState::ApplicationLaunchAttempt &attempt) {
  static_cast<void>(attempt);
  return state.application_touch_suspended &&
         state.application_suspension_reason ==
             KernelSharedState::ApplicationSuspensionReason::Home &&
         state.suspended_application_scene_process_id == process_id;
}

bool different_foreground_attempt_locked(const KernelSharedState &state,
                                         std::uint32_t process_id) {
  return state.foreground_application_attempt_process_id &&
         *state.foreground_application_attempt_process_id != process_id;
}

bool attempt_authorized_for_foreground_locked(
    const KernelSharedState &state, std::uint32_t process_id,
    const KernelSharedState::ApplicationLaunchAttempt &attempt) {
  return !attempt_interrupted(attempt) &&
         state.foreground_application_attempt_process_id == process_id &&
         (attempt.phase ==
              KernelSharedState::ApplicationLaunchPhase::Launching ||
          attempt.phase == KernelSharedState::ApplicationLaunchPhase::Active);
}

void suppress_application_fullscreen_surfaces_locked(
    KernelSharedState &state, std::uint32_t process_id) {
  if (process_id == 0U)
    return;
  state.suppress_future_application_fullscreen_surface_processes.insert(
      process_id);
  const auto publications =
      state.application_fullscreen_surface_publications.find(process_id);
  if (publications ==
      state.application_fullscreen_surface_publications.end()) {
    return;
  }
  for (const auto publication_sequence : publications->second) {
    state.suppressed_application_fullscreen_surface_publications.emplace(
        process_id, publication_sequence);
  }
  if (!publications->second.empty()) {
    state.application_fullscreen_surface_suppression_active.store(
        true, std::memory_order_release);
  }
}

void release_application_fullscreen_suppression_locked(
    KernelSharedState &state, std::uint32_t process_id) {
  state.suppress_future_application_fullscreen_surface_processes.erase(
      process_id);
  std::erase_if(
      state.suppressed_application_fullscreen_surface_publications,
      [process_id](const auto &publication) {
        return publication.first == process_id;
      });
  state.application_fullscreen_surface_suppression_active.store(
      !state.suppressed_application_fullscreen_surface_publications.empty(),
      std::memory_order_release);
}

bool held_launch_matches_locked(
    const KernelSharedState &state, std::uint32_t process_id,
    const KernelSharedState::ApplicationLaunchAttempt &attempt) {
  return state.held_application_launch &&
         state.held_application_launch->process_id == process_id &&
         state.held_application_launch->launch_token == attempt.token &&
         state.held_application_launch->origin_touch_sequence ==
             attempt.origin_touch_sequence &&
         pending_held_application_origin_locked(state, process_id).has_value();
}

void bind_held_launch_locked(
    KernelSharedState &state, std::uint32_t process_id,
    const KernelSharedState::ApplicationLaunchAttempt &attempt) {
  if (!attempt_held_by_lock(attempt) ||
      !pending_held_application_origin_locked(state, process_id) ||
      attempt.origin_touch_sequence !=
          state.held_application_launch->origin_touch_sequence) {
    return;
  }
  state.held_application_launch->process_id = process_id;
  state.held_application_launch->launch_token = attempt.token;
}

bool resume_held_launch_after_unlock_locked(
    KernelSharedState &state, std::uint32_t process_id,
    KernelSharedState::ApplicationLaunchAttempt &attempt) {
  if (!held_launch_matches_locked(state, process_id, attempt) ||
      state.held_application_launch->unlock_up_sequence == 0U) {
    return false;
  }
  attempt.phase = KernelSharedState::ApplicationLaunchPhase::Launching;
  state.foreground_application_attempt_process_id = process_id;
  release_application_fullscreen_suppression_locked(state, process_id);
  return true;
}

struct UnlockTransitionCompletion {
  bool completes_interrupted_home_exit{};
  std::optional<std::uint32_t> resume_process_id;
};

UnlockTransitionCompletion
complete_unlock_transition_locked(KernelSharedState &state,
                                  std::uint64_t unlock_up_sequence) {
  UnlockTransitionCompletion completion;
  if (!state.application_launch_barrier ||
      state.application_launch_barrier->reason !=
          KernelSharedState::ApplicationSuspensionReason::Lock) {
    state.held_application_launch.reset();
    state.interrupted_home_exit_lock_sequence.reset();
    return completion;
  }

  const auto lock_input_sequence =
      state.application_launch_barrier->input_sequence;
  completion.completes_interrupted_home_exit =
      state.interrupted_home_exit_lock_sequence &&
      *state.interrupted_home_exit_lock_sequence == lock_input_sequence;
  if (completion.completes_interrupted_home_exit)
    state.interrupted_home_exit_lock_sequence.reset();

  if (state.held_application_launch &&
      state.held_application_launch->lock_input_sequence ==
          lock_input_sequence &&
      pending_held_application_origin_locked(state)) {
    state.held_application_launch->unlock_up_sequence = unlock_up_sequence;
    const auto process_id = state.held_application_launch->process_id;
    const auto attempt = state.application_launch_attempts.find(process_id);
    if (process_id != 0U &&
        attempt != state.application_launch_attempts.end() &&
        attempt->second.token ==
            state.held_application_launch->launch_token &&
        attempt_held_by_lock(attempt->second)) {
      // The host has validated a complete rightward unlock gesture. Promote
      // the exact held token now; waiting for another lifecycle callback after
      // SpringBoard consumes Up can strand an App whose only callback raced
      // just before that receive.
      attempt->second.phase =
          KernelSharedState::ApplicationLaunchPhase::Launching;
      state.foreground_application_attempt_process_id = process_id;
      release_application_fullscreen_suppression_locked(state, process_id);
      completion.resume_process_id = process_id;
    }
  } else {
    state.held_application_launch.reset();
  }
  return completion;
}

KernelSharedState::ApplicationLaunchAttempt &begin_launch_attempt_locked(
    KernelSharedState &state, std::uint32_t process_id,
    std::uint64_t origin_touch_sequence,
    KernelSharedState::ApplicationLaunchOrigin origin) {
  auto phase = origin_touch_sequence == 0U
                   ? KernelSharedState::ApplicationLaunchPhase::Suspended
                   : KernelSharedState::ApplicationLaunchPhase::Launching;
  const auto cancelled_by_home =
      origin_touch_sequence != 0U &&
      state.last_home_launch_barrier_sequence != 0U &&
      origin_touch_sequence < state.last_home_launch_barrier_sequence;
  const auto held_origin =
      pending_held_application_origin_locked(state, process_id);
  if (cancelled_by_home) {
    phase = KernelSharedState::ApplicationLaunchPhase::InterruptedHome;
  } else if (held_origin && *held_origin == origin_touch_sequence) {
    phase = KernelSharedState::ApplicationLaunchPhase::HeldLock;
  } else if (state.application_launch_barrier &&
             state.application_launch_barrier->reason ==
                 KernelSharedState::ApplicationSuspensionReason::Lock &&
             origin_touch_sequence <
                 state.application_launch_barrier->input_sequence) {
    phase = KernelSharedState::ApplicationLaunchPhase::Suspended;
  } else if (origin_is_unlock_touch_locked(state, origin_touch_sequence)) {
    phase = KernelSharedState::ApplicationLaunchPhase::Suspended;
  }
  auto [attempt, inserted] = state.application_launch_attempts.insert_or_assign(
      process_id,
      KernelSharedState::ApplicationLaunchAttempt{
          allocate_application_launch_token_locked(state),
          origin_touch_sequence, origin, phase});
  static_cast<void>(inserted);
  if (attempt->second.phase ==
          KernelSharedState::ApplicationLaunchPhase::Launching ||
      attempt->second.phase ==
          KernelSharedState::ApplicationLaunchPhase::Active) {
    release_application_fullscreen_suppression_locked(state, process_id);
    state.foreground_application_attempt_process_id = process_id;
  } else if (attempt_held_by_lock(attempt->second)) {
    suppress_application_fullscreen_surfaces_locked(state, process_id);
    state.foreground_application_attempt_process_id = process_id;
    bind_held_launch_locked(state, process_id, attempt->second);
  } else {
    if (attempt_interrupted(attempt->second) &&
        !attempt_is_home_exit_target_locked(
            state, process_id, attempt->second)) {
      suppress_application_fullscreen_surfaces_locked(state, process_id);
    } else {
      release_application_fullscreen_suppression_locked(state, process_id);
    }
    if (state.foreground_application_attempt_process_id == process_id)
      state.foreground_application_attempt_process_id.reset();
  }
  return attempt->second;
}

void record_resident_lookup_locked(
    KernelSharedState &state, std::uint32_t process_id,
    std::uint64_t origin_touch_sequence) {
  // Unlock may cause SpringBoard to probe resident application services. It
  // is never a foreground selection, so preserve an exact existing token (or
  // leave a prewarmed process without one) until a later icon gesture.
  if (origin_is_unlock_touch_locked(state, origin_touch_sequence))
    return;
  if (const auto *existing = launch_attempt_locked(state, process_id)) {
    // Repeated lookups caused by one icon/unlock gesture are one intent. In
    // particular, unlock-triggered lookups must retain the HeldLock token.
    if (existing->origin_touch_sequence == origin_touch_sequence) {
      bind_held_launch_locked(state, process_id, *existing);
      if (state.springboard_pending_launch_touch_sequence ==
          origin_touch_sequence) {
        state.springboard_pending_launch_touch_sequence = 0U;
      }
      return;
    }
    const auto owns_live_foreground_route =
        !state.application_touch_suspended &&
        object_owned_by_process_locked(
            state, state.active_application_event_object, process_id);
    if ((existing->phase ==
             KernelSharedState::ApplicationLaunchPhase::Launching &&
         state.foreground_application_attempt_process_id == process_id) ||
        (existing->phase ==
             KernelSharedState::ApplicationLaunchPhase::Active &&
         owns_live_foreground_route) ||
        existing->phase ==
            KernelSharedState::ApplicationLaunchPhase::HeldLock) {
      return;
    }
  }
  static_cast<void>(begin_launch_attempt_locked(
      state, process_id, origin_touch_sequence,
      KernelSharedState::ApplicationLaunchOrigin::EventServiceLookup));
  if (state.springboard_pending_launch_touch_sequence ==
      origin_touch_sequence) {
    state.springboard_pending_launch_touch_sequence = 0U;
  }
}

void apply_launch_barrier_locked(
    KernelSharedState &state,
    KernelSharedState::ApplicationSuspensionReason reason,
    std::uint64_t input_sequence,
    std::optional<std::uint32_t> sampleable_home_exit_process_id) {
  const auto prior_held_application_launch =
      state.held_application_launch;
  state.application_launch_barrier =
      KernelSharedState::ApplicationLaunchBarrier{reason, input_sequence};
  state.interrupted_home_exit_lock_sequence.reset();

  if (reason == KernelSharedState::ApplicationSuspensionReason::Home) {
    state.last_home_launch_barrier_sequence =
        std::max(state.last_home_launch_barrier_sequence, input_sequence);
    state.held_application_launch.reset();
    state.springboard_pending_launch_touch_sequence = 0U;
    for (auto &[process_id, attempt] : state.application_launch_attempts) {
      const auto selected =
          state.foreground_application_attempt_process_id == process_id;
      if (selected &&
          (attempt.phase ==
               KernelSharedState::ApplicationLaunchPhase::Launching ||
           attempt.phase ==
               KernelSharedState::ApplicationLaunchPhase::HeldLock) &&
          attempt.origin_touch_sequence < input_sequence) {
        attempt.phase =
            KernelSharedState::ApplicationLaunchPhase::InterruptedHome;
        // SpringBoard may defer Home until the App publishes its first real
        // frame. Keep that exact outgoing surface sampleable for the native
        // shrink animation; foreground authorization, not black pixels,
        // prevents the cancelled App from taking ownership again.
        if (sampleable_home_exit_process_id == process_id) {
          release_application_fullscreen_suppression_locked(state,
                                                            process_id);
        } else {
          suppress_application_fullscreen_surfaces_locked(state, process_id);
        }
        state.foreground_application_attempt_process_id.reset();
      } else if (attempt.phase !=
                 KernelSharedState::ApplicationLaunchPhase::Active) {
        if (attempt.phase ==
                KernelSharedState::ApplicationLaunchPhase::Launching &&
            attempt.origin_touch_sequence < input_sequence) {
          attempt.phase = KernelSharedState::ApplicationLaunchPhase::Suspended;
        }
        if (attempt_interrupted(attempt) &&
            sampleable_home_exit_process_id != process_id) {
          suppress_application_fullscreen_surfaces_locked(state, process_id);
        } else {
          release_application_fullscreen_suppression_locked(state,
                                                            process_id);
        }
      }
    }
    return;
  }

  const auto active_foreground =
      !state.application_touch_suspended &&
      state.active_application_event_object != 0U &&
      state.active_application_scene &&
      state.active_application_scene->event_object ==
          state.active_application_event_object &&
      object_owned_by_process_locked(
          state, state.active_application_event_object,
          state.active_application_scene->process_id);

  std::optional<std::uint32_t> selected_process_id;
  if (state.foreground_application_attempt_process_id) {
    const auto attempt = state.application_launch_attempts.find(
        *state.foreground_application_attempt_process_id);
    if (attempt != state.application_launch_attempts.end() &&
        (attempt->second.phase ==
             KernelSharedState::ApplicationLaunchPhase::Launching ||
         attempt->second.phase ==
             KernelSharedState::ApplicationLaunchPhase::HeldLock) &&
        attempt->second.origin_touch_sequence < input_sequence) {
      selected_process_id = attempt->first;
    }
  }

  state.held_application_launch.reset();
  if (!active_foreground) {
    auto gesture_origin = std::uint64_t{};
    if (selected_process_id) {
      gesture_origin =
          state.application_launch_attempts.at(*selected_process_id)
              .origin_touch_sequence;
    } else if (prior_held_application_launch &&
               prior_held_application_launch->origin_touch_sequence != 0U &&
               prior_held_application_launch->origin_touch_sequence >=
                   state.last_home_launch_barrier_sequence) {
      gesture_origin =
          prior_held_application_launch->origin_touch_sequence;
    } else if (state.active_springboard_alert_items.empty() &&
               !state.springboard_unlock_touch_pending &&
               !state.springboard_unlock_touch_active) {
      gesture_origin =
          state.springboard_pending_launch_touch_sequence;
      if (gesture_origin == 0U)
        gesture_origin =
            state.springboard_enqueued_active_touch_begin_sequence;
      if (gesture_origin == 0U)
        gesture_origin = state.springboard_active_touch_begin_sequence;
      if (gesture_origin == 0U &&
          state.springboard_enqueued_last_touch_begin_sequence != 0U &&
          state.springboard_enqueued_last_touch_end_sequence <
              input_sequence &&
          input_sequence -
                  state.springboard_enqueued_last_touch_end_sequence <=
              2U) {
        gesture_origin =
            state.springboard_enqueued_last_touch_begin_sequence;
      }
    }
    if (gesture_origin != 0U && gesture_origin < input_sequence &&
        gesture_origin >= state.last_home_launch_barrier_sequence) {
      state.held_application_launch =
          KernelSharedState::HeldApplicationLaunch{
              gesture_origin, input_sequence, 0U, 0U, 0U};
    }
  }

  for (auto &[process_id, attempt] : state.application_launch_attempts) {
    if (attempt.phase == KernelSharedState::ApplicationLaunchPhase::Active)
      continue;
    if (attempt_interrupted(attempt)) {
      // A Lock that follows Home must not turn the already-outgoing snapshot
      // black while SpringBoard finishes the exit behind the lock screen.
      if (sampleable_home_exit_process_id == process_id) {
        release_application_fullscreen_suppression_locked(state, process_id);
      } else {
        suppress_application_fullscreen_surfaces_locked(state, process_id);
      }
      if (state.foreground_application_attempt_process_id == process_id)
        state.foreground_application_attempt_process_id.reset();
      continue;
    }
    if (selected_process_id == process_id &&
        attempt.origin_touch_sequence < input_sequence &&
        attempt.origin_touch_sequence >=
            state.last_home_launch_barrier_sequence) {
      attempt.phase = KernelSharedState::ApplicationLaunchPhase::HeldLock;
      suppress_application_fullscreen_surfaces_locked(state, process_id);
      state.held_application_launch =
          KernelSharedState::HeldApplicationLaunch{
              attempt.origin_touch_sequence, input_sequence, process_id,
              attempt.token, 0U};
      state.foreground_application_attempt_process_id = process_id;
      continue;
    }
    if (attempt.phase == KernelSharedState::ApplicationLaunchPhase::Launching &&
        attempt.origin_touch_sequence < input_sequence)
      attempt.phase = KernelSharedState::ApplicationLaunchPhase::Suspended;
    release_application_fullscreen_suppression_locked(state, process_id);
    if (state.foreground_application_attempt_process_id == process_id)
      state.foreground_application_attempt_process_id.reset();
  }
}

void suspend_interrupted_process_locked(
    KernelSharedState &state, std::uint32_t process_id,
    KernelSharedState::ApplicationSuspensionReason reason,
    SceneCoordinator *scenes) {
  if (scenes)
    scenes->suspend_client_scene(process_id);

  const auto owns_global_scene =
      state.active_application_scene &&
      state.active_application_scene->process_id == process_id;
  const auto owns_global_route = object_owned_by_process_locked(
      state, state.active_application_event_object, process_id);
  if (!owns_global_scene && !owns_global_route)
    return;

  state.application_touch_suspended = true;
  state.application_suspension_reason = reason;
  state.suspended_application_scene_process_id = process_id;
}

void maintain_home_exit_process_locked(KernelSharedState &state,
                                       std::uint32_t process_id,
                                       SceneCoordinator *scenes) {
  release_application_fullscreen_suppression_locked(state, process_id);
  state.application_touch_suspended = true;
  state.application_suspension_reason =
      KernelSharedState::ApplicationSuspensionReason::Home;
  state.suspended_application_scene_process_id = process_id;
  if (scenes)
    scenes->begin_client_scene_exit(process_id);

  auto event_object = std::uint32_t{};
  if (const auto pending = state.mach_port_objects.lookup(
          state.pending_application_event_object);
      pending && pending->receive_owner == process_id) {
    event_object = state.pending_application_event_object;
  } else if (state.active_application_scene &&
             state.active_application_scene->process_id == process_id) {
    event_object = state.active_application_scene->event_object;
  }
  if (event_object == 0U)
    return;

  std::optional<KernelSharedState::ApplicationTouchTransform> transform;
  if (state.latest_application_scene_transform &&
      state.latest_application_scene_transform->process_id == process_id) {
    transform = state.latest_application_scene_transform->transform;
  } else if (const auto cached =
                 state.application_scene_transforms.find(process_id);
             cached != state.application_scene_transforms.end()) {
    transform = cached->second;
  } else if (state.active_application_scene &&
             state.active_application_scene->process_id == process_id) {
    transform = state.active_application_scene->touch_transform;
  }
  state.active_application_scene = KernelSharedState::ActiveApplicationScene{
      process_id, event_object, transform};
  state.active_application_event_object = event_object;
}

void complete_home_transition_locked(KernelSharedState &state,
                                     std::uint32_t process_id,
                                     SceneCoordinator *scenes) {
  if (scenes)
    scenes->suspend_client_scene(process_id);
  if (state.active_application_scene &&
      state.active_application_scene->process_id == process_id) {
    state.active_application_scene.reset();
  }
  if (object_owned_by_process_locked(
          state, state.active_application_event_object, process_id)) {
    state.active_application_event_object = 0U;
  }
  if (object_owned_by_process_locked(
          state, state.pending_application_event_object, process_id)) {
    state.pending_application_event_object = 0U;
  }
  if (auto *attempt = launch_attempt_locked(state, process_id); attempt) {
    attempt->phase = KernelSharedState::ApplicationLaunchPhase::Suspended;
  }
  if (state.latest_application_scene_transform &&
      state.latest_application_scene_transform->process_id == process_id) {
    state.latest_application_scene_transform.reset();
  }
  std::erase_if(state.application_scene_context_owners,
                [process_id](const auto &owner) {
                  return owner.second == process_id;
                });
  if (state.held_application_launch &&
      state.held_application_launch->process_id == process_id) {
    state.held_application_launch.reset();
  }
  if (state.foreground_application_attempt_process_id == process_id)
    state.foreground_application_attempt_process_id.reset();
  release_application_fullscreen_suppression_locked(state, process_id);
  state.application_touch_suspended = false;
  state.application_suspension_reason =
      KernelSharedState::ApplicationSuspensionReason::None;
  state.suspended_application_scene_process_id.reset();
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
                                    std::string_view service_name,
                                    std::uint32_t requester_process_id) {
  if (reply_object != 0 && !service_name.empty()) {
    const auto origin_touch_sequence =
        springboard_launch_origin_touch_sequence_locked(state);
    state.pending_bootstrap_service_lookups[reply_object] =
        KernelSharedState::PendingBootstrapServiceLookup{
            std::string{service_name}, requester_process_id,
            origin_touch_sequence,
            process_is_springboard_locked(state, requester_process_id) &&
                !state.springboard_unlock_touch_pending &&
                !state.springboard_unlock_touch_active &&
                origin_touch_sequence != 0U};
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

  const auto lookup = std::move(pending->second);
  const auto service_name = lookup.service_name;
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
        queue_locked(state, service->object, input.touch,
                     input.input_sequence);
      } else {
        queue_simple_event_locked(state, service->object,
                                  input.system_event_type,
                                  input.input_sequence, input.input_kind);
      }
      state.pending_graphics_inputs.pop_front();
      ++flushed;
    }
  } else if (const auto port =
                 state.mach_port_objects.lookup(service->object)) {
    const auto process = state.processes.find(port->receive_owner);
    if (process != state.processes.end() && !process->second.exited &&
        process->second.executable_path.starts_with("/Applications/")) {
      const auto exact_springboard_request =
          lookup.requester_process_id == receiver &&
          process_is_springboard_locked(state, receiver);
      if (exact_springboard_request) {
        const auto *existing =
            launch_attempt_locked(state, port->receive_owner);
        const auto exact_existing_intent =
            existing &&
            (lookup.origin_touch_sequence == 0U ||
             existing->origin_touch_sequence ==
                 lookup.origin_touch_sequence) &&
            (state.foreground_application_attempt_process_id ==
                 port->receive_owner ||
             attempt_held_by_lock(*existing) ||
             attempt_is_home_exit_target_locked(
                 state, port->receive_owner, *existing));
        const auto effective_origin_touch_sequence =
            lookup.origin_touch_sequence != 0U
                ? lookup.origin_touch_sequence
                : exact_existing_intent
                      ? existing->origin_touch_sequence
                      : 0U;
        const auto exact_pending_gesture =
            lookup.application_launch_candidate &&
            state.springboard_pending_launch_touch_sequence != 0U &&
            state.springboard_pending_launch_touch_sequence ==
                effective_origin_touch_sequence;
        if (exact_existing_intent || exact_pending_gesture) {
          record_resident_lookup_locked(
              state, port->receive_owner,
              effective_origin_touch_sequence);
          const auto *attempt =
              launch_attempt_locked(state, port->receive_owner);
          const auto accepted_foreground_route =
              attempt &&
              attempt->origin_touch_sequence ==
                  effective_origin_touch_sequence &&
              (state.foreground_application_attempt_process_id ==
                   port->receive_owner ||
               attempt_held_by_lock(*attempt) ||
               attempt_is_home_exit_target_locked(
                   state, port->receive_owner, *attempt));
          if (accepted_foreground_route) {
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
      }
    }
  }
  return ServiceResolution{service->object, flushed, application_event_port,
                           service_name};
}

EnqueueResult enqueue_touch(KernelSharedState &state, const TouchInput &input,
                            SceneCoordinator *scenes,
                            bool *home_recovery_requested) {
  if (home_recovery_requested)
    *home_recovery_requested = false;
  const TouchInput sanitized{input.phase,
                             std::isfinite(input.x) ? input.x : 0.0F,
                             std::isfinite(input.y) ? input.y : 0.0F};
  std::unique_lock lock{state.mach_mutex};
  const auto input_sequence =
      allocate_graphics_input_sequence_locked(state);
  if (!state.springboard_unlock_touch_pending &&
      !state.springboard_unlock_touch_active &&
      state.active_springboard_alert_items.empty() &&
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
          clear_springboard_enqueued_gesture_locked(state);
          queue_locked(state, state.active_application_event_object,
                       application_input, input_sequence);
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
        clear_springboard_enqueued_gesture_locked(state);
        queue_locked(state, state.active_application_event_object,
                     application_input, input_sequence);
        return EnqueueResult::Queued;
      }
    } else {
      state.active_application_event_object = 0U;
      state.application_touch_suspended = false;
    }
  }
  const auto unlock_touch_input =
      state.springboard_unlock_touch_pending ||
      state.springboard_unlock_touch_active;
  auto completed_unlock_gesture = false;
  if (state.springboard_unlock_touch_pending &&
      sanitized.phase == TouchPhase::Down) {
    state.springboard_unlock_touch_pending = false;
    state.springboard_unlock_touch_active = true;
    state.springboard_unlock_touch_begin_sequence = input_sequence;
    state.springboard_unlock_touch_end_sequence = 0U;
    state.springboard_unlock_touch_start_x = sanitized.x;
    state.springboard_unlock_touch_start_y = sanitized.y;
  } else if (state.springboard_unlock_touch_active &&
             (sanitized.phase == TouchPhase::Up ||
              sanitized.phase == TouchPhase::Cancel)) {
    state.springboard_unlock_touch_active = false;
    state.springboard_unlock_touch_end_sequence = input_sequence;
    const auto horizontal_distance =
        sanitized.x - state.springboard_unlock_touch_start_x;
    const auto vertical_distance =
        std::fabs(sanitized.y -
                  state.springboard_unlock_touch_start_y);
    // The iPhone OS 1.x lock control is a deliberate rightward slider. Do not
    // turn a tap, failed drag, or cancelled gesture into a synthetic Home.
    completed_unlock_gesture =
        sanitized.phase == TouchPhase::Up &&
        horizontal_distance >= 96.0F &&
        vertical_distance <=
            std::max(64.0F, horizontal_distance * 0.75F);
    state.springboard_unlock_touch_pending =
        !completed_unlock_gesture;
  }
  if (!unlock_touch_input) {
    if (sanitized.phase == TouchPhase::Down) {
      if (state.application_touch_suspended &&
          state.application_suspension_reason ==
              KernelSharedState::ApplicationSuspensionReason::Home &&
          state.suspended_application_scene_process_id) {
        complete_home_transition_locked(
            state, *state.suspended_application_scene_process_id, scenes);
      }
      // A deliberate post-unlock gesture supersedes a held launch that still
      // has not become foreground. The exact PID/token remains suppressed; a
      // later lookup for it is background work, not the user's new selection.
      if (state.held_application_launch &&
          state.held_application_launch->unlock_up_sequence != 0U) {
        const auto held = *state.held_application_launch;
        const auto attempt =
            state.application_launch_attempts.find(held.process_id);
        if (held.process_id != 0U &&
            attempt != state.application_launch_attempts.end() &&
            attempt->second.token == held.launch_token &&
            attempt->second.phase !=
                KernelSharedState::ApplicationLaunchPhase::Active) {
          attempt->second.phase =
              KernelSharedState::ApplicationLaunchPhase::Suspended;
          suppress_application_fullscreen_surfaces_locked(
              state, held.process_id);
          if (state.foreground_application_attempt_process_id ==
              held.process_id) {
            state.foreground_application_attempt_process_id.reset();
          }
        }
        state.held_application_launch.reset();
      }
      state.springboard_pending_launch_touch_sequence = input_sequence;
      state.springboard_enqueued_active_touch_begin_sequence =
          input_sequence;
    } else if (sanitized.phase == TouchPhase::Up ||
               sanitized.phase == TouchPhase::Cancel) {
      state.springboard_enqueued_last_touch_begin_sequence =
          state.springboard_enqueued_active_touch_begin_sequence != 0U
              ? state.springboard_enqueued_active_touch_begin_sequence
              : input_sequence;
      state.springboard_enqueued_last_touch_end_sequence = input_sequence;
      state.springboard_enqueued_active_touch_begin_sequence = 0U;
    }
  }
  auto unlock_completion = UnlockTransitionCompletion{};
  if (completed_unlock_gesture) {
    unlock_completion =
        complete_unlock_transition_locked(state, input_sequence);
  }
  if (unlock_completion.completes_interrupted_home_exit &&
      home_recovery_requested) {
    *home_recovery_requested = true;
  }
  const auto resume_process_id =
      unlock_completion.resume_process_id;
  const auto service =
      state.bootstrap_service_objects.find(std::string{system_event_service});
  if (service == state.bootstrap_service_objects.end()) {
    state.pending_graphics_inputs.push_back(
        KernelSharedState::PendingGraphicsInput{
            KernelSharedState::PendingGraphicsInput::Kind::Touch, sanitized,
            0, input_sequence,
            KernelSharedState::MachMessage::GraphicsInputKind::Touch});
    lock.unlock();
    if (resume_process_id) {
      activate_resolved_application(state, *resume_process_id, scenes);
    }
    return EnqueueResult::Deferred;
  }
  queue_locked(state, service->second, sanitized, input_sequence);
  lock.unlock();
  if (resume_process_id) {
    activate_resolved_application(state, *resume_process_id, scenes);
  }
  return EnqueueResult::Queued;
}

EnqueueResult enqueue_system_button(KernelSharedState &state,
                                    const SystemButtonInput &input,
                                    std::uint64_t *input_sequence,
                                    bool begins_display_lock_transaction) {
  const auto event_type = system_button_event_type(input);
  const auto input_kind = system_button_input_kind(input.button);
  std::lock_guard lock{state.mach_mutex};
  const auto sequence = allocate_graphics_input_sequence_locked(state);
  if (input_sequence)
    *input_sequence = sequence;
  if (begins_display_lock_transaction) {
    state.host_display_current_lock_down_sequence = sequence;
    state.host_display_pending_lock_power_off_sequences.push_back(sequence);
  }
  const auto service =
      state.bootstrap_service_objects.find(std::string{system_event_service});
  if (service == state.bootstrap_service_objects.end()) {
    state.pending_graphics_inputs.push_back(
        KernelSharedState::PendingGraphicsInput{
            KernelSharedState::PendingGraphicsInput::Kind::SystemEvent,
            {},
            event_type, sequence, input_kind});
    return EnqueueResult::Deferred;
  }
  queue_simple_event_locked(state, service->second, event_type, sequence,
                            input_kind);
  return EnqueueResult::Queued;
}

void record_lock_wake_request(KernelSharedState &state) {
  std::lock_guard lock{state.mach_mutex};
  if (state.springboard_unlock_touch_pending ||
      state.springboard_unlock_touch_active) {
    return;
  }
  state.springboard_unlock_touch_pending = true;
}

EnqueueResult enqueue_ringer_switch_change(KernelSharedState &state,
                                           bool active) {
  const auto event_type =
      active ? ringer_switch_on_event_type : ringer_switch_off_event_type;
  std::lock_guard lock{state.mach_mutex};
  const auto input_sequence =
      allocate_graphics_input_sequence_locked(state);
  constexpr auto input_kind =
      KernelSharedState::MachMessage::GraphicsInputKind::OtherSystem;
  const auto service =
      state.bootstrap_service_objects.find(std::string{system_event_service});
  if (service == state.bootstrap_service_objects.end()) {
    state.pending_graphics_inputs.push_back(
            KernelSharedState::PendingGraphicsInput{
                KernelSharedState::PendingGraphicsInput::Kind::SystemEvent,
                {},
                event_type, input_sequence, input_kind});
    return EnqueueResult::Deferred;
  }
  queue_simple_event_locked(state, service->second, event_type,
                            input_sequence, input_kind);
  return EnqueueResult::Queued;
}

void suspend_active_application(
    KernelSharedState &state,
    KernelSharedState::ApplicationSuspensionReason reason,
    SceneCoordinator *scenes, std::uint64_t system_input_sequence) {
  std::lock_guard lock{state.mach_mutex};
  if (system_input_sequence == 0U) {
    system_input_sequence =
        allocate_graphics_input_sequence_locked(state);
  }
  const auto prior_home_exit_process_id =
      state.application_touch_suspended &&
              state.application_suspension_reason ==
                  KernelSharedState::ApplicationSuspensionReason::Home
          ? state.suspended_application_scene_process_id
          : std::nullopt;

  std::optional<std::uint32_t> target_process_id;
  if (state.foreground_application_attempt_process_id) {
    const auto process =
        state.processes.find(*state.foreground_application_attempt_process_id);
    if (process != state.processes.end() && !process->second.exited &&
        launch_attempt_locked(
            state, *state.foreground_application_attempt_process_id)) {
      target_process_id = process->first;
    } else {
      state.foreground_application_attempt_process_id.reset();
    }
  }

  std::optional<std::uint32_t> routed_process_id;
  if (const auto active_port = state.mach_port_objects.lookup(
          state.active_application_event_object)) {
    routed_process_id = active_port->receive_owner;
  }
  const auto active_process_id =
      state.active_application_scene
          ? std::optional<std::uint32_t>{
                state.active_application_scene->process_id}
          : routed_process_id;
  if (!target_process_id &&
      reason == KernelSharedState::ApplicationSuspensionReason::Lock &&
      state.application_touch_suspended &&
      state.application_suspension_reason ==
          KernelSharedState::ApplicationSuspensionReason::Home &&
      state.suspended_application_scene_process_id &&
      ((state.active_application_scene &&
        state.active_application_scene->process_id ==
            *state.suspended_application_scene_process_id) ||
       routed_process_id ==
           state.suspended_application_scene_process_id)) {
    target_process_id =
        *state.suspended_application_scene_process_id;
  }
  if (!target_process_id && active_process_id &&
      state.active_application_event_object != 0U &&
      !state.application_touch_suspended &&
      (!scenes || scenes->client_scene_active(*active_process_id))) {
    target_process_id = active_process_id;
  }
  // Capture the exact selected token before Home updates the cancellation
  // watermark and clears foreground authorization. This narrow interval is
  // where SpringBoard has finished the opening animation but the App has not
  // yet published its first UI frame.
  const auto sampleable_home_exit_process_id =
      reason == KernelSharedState::ApplicationSuspensionReason::Home
          ? target_process_id ? target_process_id : prior_home_exit_process_id
          : prior_home_exit_process_id;
  apply_launch_barrier_locked(state, reason, system_input_sequence,
                              sampleable_home_exit_process_id);

  // With no exact current target, the sequence barrier is sufficient. A later
  // SpringBoard spawn/lookup will create a PID-bound attempt and compare its
  // causal touch sequence with this barrier.
  if (!target_process_id)
    return;

  if (prior_home_exit_process_id == target_process_id) {
    state.interrupted_home_exit_lock_sequence =
        system_input_sequence;
  }
  state.application_touch_suspended = true;
  state.application_suspension_reason = reason;
  state.suspended_application_scene_process_id = *target_process_id;
  if (reason == KernelSharedState::ApplicationSuspensionReason::Home) {
    release_application_fullscreen_suppression_locked(
        state, *target_process_id);
  }
  if (scenes) {
    if (reason ==
        KernelSharedState::ApplicationSuspensionReason::Home) {
      scenes->begin_client_scene_exit(*target_process_id);
    } else {
      scenes->suspend_client_scene(*target_process_id);
    }
  }
}

void complete_home_transition_after_present(
    KernelSharedState &state, std::uint32_t presenter_process_id,
    SceneCoordinator *scenes) {
  std::lock_guard lock{state.mach_mutex};
  if (!process_is_springboard_locked(state, presenter_process_id) ||
      !state.application_touch_suspended ||
      state.application_suspension_reason !=
          KernelSharedState::ApplicationSuspensionReason::Home ||
      !state.suspended_application_scene_process_id) {
    return;
  }
  const auto process_id =
      *state.suspended_application_scene_process_id;
  // The first SpringBoard SwapEnd starts the animation; it is not an ownership
  // completion point. Keep any live, PID-bound attempt sampleable until the
  // ordered 2003 background event. A later user gesture provides a
  // deterministic stale-exit fallback if that private event never arrives.
  const auto process = state.processes.find(process_id);
  if (launch_attempt_locked(state, process_id) &&
      process != state.processes.end() && !process->second.exited) {
    return;
  }
  complete_home_transition_locked(state, process_id, scenes);
}

void record_application_spawn(
    KernelSharedState &state, std::uint32_t sender_process_id,
    std::uint32_t process_id, std::string_view executable_path,
    std::span<const std::string> arguments, SceneCoordinator *scenes) {
  std::lock_guard lock{state.mach_mutex};
  if (!process_is_springboard_locked(state, sender_process_id) ||
      process_id == 0U ||
      !executable_path.starts_with("/Applications/") ||
      std::find(arguments.begin(), arguments.end(), "--suspended") !=
          arguments.end()) {
    return;
  }
  const auto held_origin =
      pending_held_application_origin_locked(state, process_id);
  const auto origin_touch_sequence =
      held_origin.value_or(
          springboard_launch_origin_touch_sequence_locked(state));
  auto *attempt = launch_attempt_locked(state, process_id);
  if (!attempt ||
      attempt->origin_touch_sequence != origin_touch_sequence) {
    attempt = &begin_launch_attempt_locked(
        state, process_id, origin_touch_sequence,
        KernelSharedState::ApplicationLaunchOrigin::Spawn);
  } else {
    bind_held_launch_locked(state, process_id, *attempt);
  }
  if (state.springboard_pending_launch_touch_sequence ==
      origin_touch_sequence) {
    state.springboard_pending_launch_touch_sequence = 0U;
  }
  if (attempt_is_home_exit_target_locked(state, process_id, *attempt)) {
    maintain_home_exit_process_locked(state, process_id, scenes);
  } else if ((attempt_interrupted(*attempt) ||
              attempt_held_by_lock(*attempt)) &&
             scenes) {
    scenes->suspend_client_scene(process_id);
  }
}

void activate_resolved_application(KernelSharedState &state,
                                   std::uint32_t process_id,
                                   SceneCoordinator *scenes) {
  std::lock_guard lock{state.mach_mutex};
  auto *attempt = launch_attempt_locked(state, process_id);
  if (!attempt) {
    if (scenes)
      scenes->suspend_client_scene(process_id);
    return;
  }
  if (attempt_is_home_exit_target_locked(state, process_id, *attempt)) {
    maintain_home_exit_process_locked(state, process_id, scenes);
    return;
  }
  if (attempt_held_by_lock(*attempt) &&
      !resume_held_launch_after_unlock_locked(state, process_id, *attempt)) {
    if (scenes)
      scenes->suspend_client_scene(process_id);
    state.application_touch_suspended = true;
    state.application_suspension_reason =
        KernelSharedState::ApplicationSuspensionReason::Lock;
    state.suspended_application_scene_process_id = process_id;
    return;
  }
  if (attempt_interrupted(*attempt)) {
    suspend_interrupted_process_locked(
        state, process_id, interruption_reason(attempt->phase), scenes);
    return;
  }
  const auto resumes_locked_scene =
      attempt->phase == KernelSharedState::ApplicationLaunchPhase::Suspended &&
      state.application_touch_suspended &&
      state.application_suspension_reason ==
          KernelSharedState::ApplicationSuspensionReason::Lock &&
      state.suspended_application_scene_process_id == process_id;
  if (!attempt_authorized_for_foreground_locked(state, process_id, *attempt) &&
      !resumes_locked_scene) {
    if (scenes)
      scenes->suspend_client_scene(process_id);
    return;
  }
  if (different_foreground_attempt_locked(state, process_id)) {
    if (scenes)
      scenes->suspend_client_scene(process_id);
    return;
  }
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
    attempt->phase = KernelSharedState::ApplicationLaunchPhase::Active;
    clear_springboard_enqueued_gesture_locked(state);
    if (held_launch_matches_locked(state, process_id, *attempt))
      state.held_application_launch.reset();
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
  auto owner = state.application_scene_context_owners.find(context_key);
  const auto exact_scene_target = [&state](std::uint32_t candidate) {
    const auto *attempt = launch_attempt_locked(state, candidate);
    if (!attempt)
      return false;
    const auto exact_held_launch =
        attempt_held_by_lock(*attempt) &&
        held_launch_matches_locked(state, candidate, *attempt);
    const auto exact_home_exit =
        attempt_is_home_exit_target_locked(state, candidate, *attempt);
    return (attempt_authorized_for_foreground_locked(
                state, candidate, *attempt) ||
            exact_held_launch || exact_home_exit) &&
           !different_foreground_attempt_locked(state, candidate);
  };
  const auto exact_unbound_target = [&state, &exact_scene_target,
                                     render_process_id]()
      -> std::optional<std::uint32_t> {
    if (!process_is_springboard_locked(state, render_process_id))
      return std::nullopt;
    if (state.foreground_application_attempt_process_id &&
        exact_scene_target(
            *state.foreground_application_attempt_process_id)) {
      return state.foreground_application_attempt_process_id;
    }
    if (state.application_touch_suspended &&
        state.application_suspension_reason ==
            KernelSharedState::ApplicationSuspensionReason::Home &&
        state.suspended_application_scene_process_id &&
        exact_scene_target(
            *state.suspended_application_scene_process_id)) {
      return state.suspended_application_scene_process_id;
    }
    return std::nullopt;
  };

  std::uint32_t process_id{};
  if (owner != state.application_scene_context_owners.end() &&
      exact_scene_target(owner->second)) {
    process_id = owner->second;
  }
  if (process_id == 0U) {
    const auto pending_port = state.mach_port_objects.lookup(
        state.pending_application_event_object);
    if (pending_port && exact_scene_target(pending_port->receive_owner))
      process_id = pending_port->receive_owner;
  }
  if (process_id == 0U) {
    const auto unbound_target = exact_unbound_target();
    if (!unbound_target)
      return std::nullopt;
    process_id = *unbound_target;
  }
  if (owner != state.application_scene_context_owners.end() &&
      owner->second != process_id) {
    // LayerKit contexts are reused by SpringBoard. A retired owner must not
    // permanently redirect a new exact launch to the previous App.
    owner->second = process_id;
  }
  // SpringBoard publishes separate full-screen roots for the outgoing App's
  // lock screen and exit snapshots. Ignore only that App's roots: a different
  // App can publish its first live root before didBecomeActive arrives.
  const auto *attempt = launch_attempt_locked(state, process_id);
  const auto exact_held_launch =
      attempt && attempt_held_by_lock(*attempt) &&
      held_launch_matches_locked(state, process_id, *attempt);
  const auto exact_home_exit =
      attempt &&
      attempt_is_home_exit_target_locked(state, process_id, *attempt);
  const auto reactivation_in_progress =
      attempt &&
      (attempt_authorized_for_foreground_locked(state, process_id, *attempt) ||
       exact_held_launch || exact_home_exit);
  if (state.application_touch_suspended &&
      state.suspended_application_scene_process_id == process_id &&
      !reactivation_in_progress) {
    return std::nullopt;
  }
  const auto process = state.processes.find(process_id);
  if (process == state.processes.end() || process->second.exited ||
      !process->second.executable_path.starts_with("/Applications/")) {
    return std::nullopt;
  }
  if (!attempt ||
      (!attempt_authorized_for_foreground_locked(state, process_id,
                                                 *attempt) &&
       !exact_held_launch && !exact_home_exit) ||
      different_foreground_attempt_locked(state, process_id)) {
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
  if (owns_active_route && !exact_held_launch) {
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
  if (state.foreground_application_attempt_process_id == process_id) {
    state.foreground_application_attempt_process_id.reset();
  }
  if (state.held_application_launch &&
      state.held_application_launch->process_id == process_id) {
    if (state.held_application_launch->unlock_up_sequence == 0U &&
        state.held_application_launch->origin_touch_sequence >=
            state.last_home_launch_barrier_sequence) {
      state.held_application_launch->process_id = 0U;
      state.held_application_launch->launch_token = 0U;
    } else {
      state.held_application_launch.reset();
    }
  }
  release_application_fullscreen_suppression_locked(state, process_id);
  state.application_launch_attempts.erase(process_id);
  state.application_fullscreen_surface_publications.erase(process_id);
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
    SceneCoordinator *scenes) {
  if (event_type == application_did_finish_background_event_type) {
    const auto sender = state.processes.find(sender_pid);
    const auto destination_port =
        state.mach_port_objects.lookup(destination);
    const auto valid_background_completion =
        sender != state.processes.end() && !sender->second.exited &&
        sender->second.executable_path.starts_with("/Applications/") &&
        destination_port &&
        process_is_springboard_locked(
            state, destination_port->receive_owner);
    if (!valid_background_completion)
      return;
    if (state.application_touch_suspended &&
        state.application_suspension_reason ==
            KernelSharedState::ApplicationSuspensionReason::Home &&
        state.suspended_application_scene_process_id == sender_pid) {
      complete_home_transition_locked(state, sender_pid, scenes);
    }
    return;
  }
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
    const auto process_id = destination_port->receive_owner;
    const auto requests_userspace_prewarm =
        std::find(application->second.arguments.begin(),
                  application->second.arguments.end(), "--suspended") !=
        application->second.arguments.end();
    // The first --suspended activation is protocol bookkeeping, not a
    // foreground attempt. Consume it before consulting interruption state so a
    // system barrier cannot turn the first real Phone open into "prewarm".
    if (requests_userspace_prewarm &&
        state.consumed_application_prewarm_activations
            .insert(process_id)
            .second) {
      return;
    }
    auto *attempt = launch_attempt_locked(state, process_id);
    if (!attempt) {
      if (scenes)
        scenes->suspend_client_scene(process_id);
      if (state.latest_application_scene_transform &&
          state.latest_application_scene_transform->process_id == process_id) {
        state.latest_application_scene_transform.reset();
      }
      return;
    }
    if (attempt_is_home_exit_target_locked(state, process_id, *attempt)) {
      state.pending_application_event_object = destination;
      maintain_home_exit_process_locked(state, process_id, scenes);
      return;
    }
    if (attempt_held_by_lock(*attempt)) {
      state.pending_application_event_object = destination;
      if (!resume_held_launch_after_unlock_locked(
              state, process_id, *attempt)) {
        if (scenes)
          scenes->suspend_client_scene(process_id);
        state.application_touch_suspended = true;
        state.application_suspension_reason =
            KernelSharedState::ApplicationSuspensionReason::Lock;
        state.suspended_application_scene_process_id = process_id;
        return;
      }
    }
    if (attempt_interrupted(*attempt)) {
      suspend_interrupted_process_locked(
          state, process_id, interruption_reason(attempt->phase), scenes);
      if (state.latest_application_scene_transform &&
          state.latest_application_scene_transform->process_id == process_id) {
        state.latest_application_scene_transform.reset();
      }
      return;
    }
    const auto resumes_locked_scene =
        attempt->phase ==
            KernelSharedState::ApplicationLaunchPhase::Suspended &&
        state.application_touch_suspended &&
        state.application_suspension_reason ==
            KernelSharedState::ApplicationSuspensionReason::Lock &&
        state.suspended_application_scene_process_id == process_id;
    if (!attempt_authorized_for_foreground_locked(state, process_id,
                                                  *attempt) &&
        !resumes_locked_scene) {
      if (scenes)
        scenes->suspend_client_scene(process_id);
      if (state.latest_application_scene_transform &&
          state.latest_application_scene_transform->process_id == process_id) {
        state.latest_application_scene_transform.reset();
      }
      return;
    }
    if (different_foreground_attempt_locked(state, process_id)) {
      if (scenes)
        scenes->suspend_client_scene(process_id);
      if (state.latest_application_scene_transform &&
          state.latest_application_scene_transform->process_id == process_id) {
        state.latest_application_scene_transform.reset();
      }
      return;
    }
    state.pending_application_event_object = destination;
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
    if (state.active_application_event_object != 0U) {
      attempt->phase = KernelSharedState::ApplicationLaunchPhase::Active;
      clear_springboard_enqueued_gesture_locked(state);
      if (held_launch_matches_locked(state, process_id, *attempt))
        state.held_application_launch.reset();
    }
    if (state.latest_application_scene_transform &&
        state.latest_application_scene_transform->process_id ==
            process_id) {
      state.latest_application_scene_transform.reset();
    }
  } else {
    const auto process_id = destination_port->receive_owner;
    auto *attempt = launch_attempt_locked(state, process_id);
    if (attempt &&
        attempt_is_home_exit_target_locked(state, process_id, *attempt)) {
      maintain_home_exit_process_locked(state, process_id, scenes);
      return;
    }
    if (attempt && attempt_held_by_lock(*attempt)) {
      if (scenes)
        scenes->suspend_client_scene(process_id);
      state.application_touch_suspended = true;
      state.application_suspension_reason =
          KernelSharedState::ApplicationSuspensionReason::Lock;
      state.suspended_application_scene_process_id = process_id;
      return;
    }
    if (attempt && attempt_interrupted(*attempt)) {
      const auto reason = interruption_reason(attempt->phase);
      if (scenes)
        scenes->suspend_client_scene(process_id);
      if (state.latest_application_scene_transform &&
          state.latest_application_scene_transform->process_id == process_id) {
        state.latest_application_scene_transform.reset();
      }
      std::erase_if(state.application_scene_context_owners,
                    [process_id](const auto &owner) {
                      return owner.second == process_id;
                    });

      const auto owns_global_scene =
          state.active_application_scene &&
          state.active_application_scene->process_id == process_id;
      const auto owns_global_route = object_owned_by_process_locked(
          state, state.active_application_event_object, process_id);
      if (reason ==
          KernelSharedState::ApplicationSuspensionReason::Home) {
        // This is an older Home-cancelled background App, not the exact
        // outgoing PID retained by maintain_home_exit_process_locked. Keep its
        // publications suppressed and retire any stale global route.
        if (owns_global_scene)
          state.active_application_scene.reset();
        if (owns_global_route)
          state.active_application_event_object = 0;
        if (state.suspended_application_scene_process_id == process_id) {
          state.application_touch_suspended = false;
          state.application_suspension_reason =
              KernelSharedState::ApplicationSuspensionReason::None;
          state.suspended_application_scene_process_id.reset();
        }
      } else if (owns_global_scene || owns_global_route) {
        state.application_touch_suspended = true;
        state.application_suspension_reason =
            KernelSharedState::ApplicationSuspensionReason::Lock;
        state.suspended_application_scene_process_id = process_id;
      }
      if (state.foreground_application_attempt_process_id == process_id)
        state.foreground_application_attempt_process_id.reset();
      return;
    }

    const auto owns_current_scene =
        state.active_application_scene &&
        state.active_application_scene->process_id == process_id;
    const auto owns_current_route = object_owned_by_process_locked(
        state, state.active_application_event_object, process_id);
    if (!owns_current_scene && !owns_current_route) {
      // Lifecycle messages are asynchronous. A previous App may resign after
      // a newer launch or Home exit has already become authoritative; such a
      // callback must not overwrite the singleton suspended PID/reason.
      return;
    }

    const auto prior_suspension_reason =
        state.application_suspension_reason;
    if (scenes) {
      if (prior_suspension_reason ==
          KernelSharedState::ApplicationSuspensionReason::Home) {
        scenes->begin_client_scene_exit(process_id);
      } else {
        scenes->suspend_client_scene(process_id);
      }
    }
    state.application_touch_suspended = true;
    state.suspended_application_scene_process_id = process_id;
    const auto preserve_locked_scene =
        prior_suspension_reason ==
            KernelSharedState::ApplicationSuspensionReason::Lock &&
        state.active_application_scene &&
        state.active_application_scene->process_id ==
            process_id;
    const auto preserve_home_exit_scene =
        prior_suspension_reason ==
            KernelSharedState::ApplicationSuspensionReason::Home &&
        state.active_application_scene &&
        state.active_application_scene->process_id ==
            process_id;
    if (preserve_locked_scene) {
      if (attempt) {
        attempt->phase =
            KernelSharedState::ApplicationLaunchPhase::Suspended;
      }
      state.application_suspension_reason =
          KernelSharedState::ApplicationSuspensionReason::Lock;
      return;
    }
    if (preserve_home_exit_scene) {
      state.application_suspension_reason =
          KernelSharedState::ApplicationSuspensionReason::Home;
      if (state.foreground_application_attempt_process_id == process_id)
        state.foreground_application_attempt_process_id.reset();
      return;
    }
    if (attempt) {
      attempt->phase =
          KernelSharedState::ApplicationLaunchPhase::Suspended;
    }
    // A normally active App remains the source of SpringBoard's shrinking
    // exit snapshot after willResignActive. Suppression is reserved for an
    // interrupted launch above; hiding this surface here turns the otherwise
    // valid Home animation black.
    state.application_suspension_reason =
        KernelSharedState::ApplicationSuspensionReason::None;
    if (state.latest_application_scene_transform &&
        state.latest_application_scene_transform->process_id ==
          process_id) {
      state.latest_application_scene_transform.reset();
    }
    for (auto context = state.application_scene_context_owners.begin();
         context != state.application_scene_context_owners.end();) {
      if (context->second == process_id) {
        context = state.application_scene_context_owners.erase(context);
      } else {
        ++context;
      }
    }
    if (state.active_application_scene &&
        state.active_application_scene->process_id == process_id) {
      state.active_application_scene.reset();
    }
    if (object_owned_by_process_locked(
            state, state.active_application_event_object, process_id)) {
      state.active_application_event_object = 0;
    }
    if (state.foreground_application_attempt_process_id == process_id)
      state.foreground_application_attempt_process_id.reset();
    state.application_touch_suspended = false;
    state.suspended_application_scene_process_id.reset();
  }
}

} // namespace ilemu::graphics_services_input
