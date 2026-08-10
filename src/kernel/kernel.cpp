#include "ilemu/kernel.hpp"
#include "ilemu/application_path.hpp"
#include "ilemu/application_display_profile.hpp"
#include "ilemu/darwin_kernel_profile.hpp"
#include "ilemu/rootfs_path_resolver.hpp"

#include "ilemu/app_support_hle.hpp"
#include "ilemu/bluetooth_manager_hle.hpp"
#include "ilemu/bootstrap_mig_ids.hpp"
#include "ilemu/core_telephony_hle.hpp"
#include "ilemu/core_animation_remote_profile.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/dns_configuration_hle.hpp"
#include "ilemu/darwin_kqueue_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
#include "ilemu/graphics_services_input.hpp"
#include "ilemu/graphics_services_profile.hpp"
#include "ilemu/iokit_abi.hpp"
#include "ilemu/kernel_bsd_interval_timer.hpp"
#include "ilemu/kernel_clock.hpp"
#include "ilemu/kernel_iokit.hpp"
#include "ilemu/kernel_iokit_camera.hpp"
#include "ilemu/kernel_iokit_display.hpp"
#include "ilemu/lockdown_hle.hpp"
#include "ilemu/kernel_mach_ipc.hpp"
#include "ilemu/kernel_mach_task_identity.hpp"
#include "ilemu/kernel_network.hpp"
#include "ilemu/mach_clock_abi.hpp"
#include "ilemu/mach_host_mig_ids.hpp"
#include "ilemu/mach_port_mig_ids.hpp"
#include "ilemu/mach_scheduler_abi.hpp"
#include "ilemu/mach_thread_policy_abi.hpp"
#include "ilemu/offline_serial_device.hpp"
#include "ilemu/macho.hpp"
#include "ilemu/mbx_connect_hle.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/performance.hpp"
#include "ilemu/task_mig_ids.hpp"
#include "ilemu/thread_act_mig_ids.hpp"
#include "ilemu/vm_map_mig_ids.hpp"
#include "ilemu/xnu_mig_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include "mach/support.hpp"

namespace ilemu {

std::size_t CompatibilityKernel::bootstrap_checked_in_service_count() const {
  std::lock_guard lock{shared_state_->mach_mutex};
  return shared_state_->bootstrap_checked_in_services.size();
}

namespace {

constexpr std::uint32_t carry_flag = 1U << 29U;
constexpr std::uint32_t ebadf = 9;
constexpr std::uint32_t efault = 14;
constexpr std::uint32_t virtual_disk_major = 14;
constexpr std::uint32_t root_disk_minor = 1;
constexpr std::uint32_t root_disk_device =
    (virtual_disk_major << 24U) | root_disk_minor;

OpenGlesGuestProfileKind open_gles_profile_for_device(
    GraphicsAcceleratorProfileKind accelerator) {
  switch (accelerator) {
  case GraphicsAcceleratorProfileKind::MbxLite:
    return OpenGlesGuestProfileKind::MbxLiteLegacy;
  case GraphicsAcceleratorProfileKind::Sgx535:
    return OpenGlesGuestProfileKind::Sgx535;
  }
  return OpenGlesGuestProfileKind::MbxLiteLegacy;
}

} // namespace

CompatibilityKernel::CompatibilityKernel(AddressSpace &memory, Output &output,
                                         std::filesystem::path rootfs,
                                         DeviceProfile device,
                                         std::optional<bool> activated,
                                         LockdownFirmwareProfile
                                             lockdown_profile)
    : memory_{memory}, output_{output}, rootfs_{std::move(rootfs)},
      device_profile_{device},
      hfs_volumes_{rootfs_, device_profile_.storage_bytes},
      hfs_metadata_{rootfs_},
      display_state_{std::make_shared<DisplayState>(device_profile_.display)},
      audio_service_{std::make_shared<AudioService>(rootfs_)},
      userland_hle_{memory_, output_},
      darwin_notify_state_hle_{userland_hle_},
      audio_toolbox_hle_{userland_hle_},
      core_media_hle_{userland_hle_},
      core_audio_hle_{userland_hle_, audio_service_},
      apple80211_hle_{
          userland_hle_, wifi_state_,
          [this](const WifiSnapshot &before, const WifiSnapshot &after) {
            apply_wifi_transition(before, after);
          }},
      core_surface_hle_{userland_hle_, display_state_, surface_store_,
                        presentation_tracker_},
      opengles_hle_{userland_hle_, display_state_, surface_store_},
      mbx2d_hle_{userland_hle_, display_state_, surface_store_,
                 presentation_tracker_},
      mobile_framebuffer_hle_{userland_hle_, display_state_, surface_store_,
                              presentation_tracker_} {
  memory_.set_file_generation_registry(
      shared_state_->guest_file_generation_registry);
  shared_state_->shared_mapping_page_cache->set_generation_registry(
      shared_state_->guest_file_generation_registry);
  opengles_hle_.set_guest_profile(
      open_gles_profile_for_device(device_profile_.graphics_accelerator));
  display_state_->set_orientation_resolver(
      [state = shared_state_, scenes = scene_coordinator_](
          std::uint32_t owner_process_id) {
        // A system compositor can publish a frame while an active client is
        // still visible (for example, a keyboard or volume HUD over a
        // landscape App). In that state the compositor's portrait process is
        // not the orientation owner of the pixels. During Home the client is
        // Exiting instead, so SpringBoard's portrait frame must win. Keep the
        // process owner as the default and use the active client only for
        // system-owned overlay publishes.
        const auto foreground_scene = scenes->foreground_client_scene();
        const auto active_scene = scenes->active_client_scene();
        std::lock_guard lock{state->mach_mutex};
        const auto owner = state->processes.find(owner_process_id);
        const auto owner_is_application =
            owner != state->processes.end() &&
            is_application_executable_path(owner->second.executable_path);
        if (!owner_is_application && active_scene &&
            active_scene->state == ClientSceneState::Active) {
          if (const auto client = state->processes.find(
                  active_scene->client_process_id);
              client != state->processes.end()) {
            return client->second.display_orientation;
          }
        }
        if (owner != state->processes.end()) {
          return owner->second.display_orientation;
        }
        if (foreground_scene) {
          if (const auto process = state->processes.find(
                  foreground_scene->client_process_id);
              process != state->processes.end()) {
            return process->second.display_orientation;
          }
        }
        return DisplayOrientation::Portrait;
      });
  apple80211_hle_.set_event_injection_handler(
      [this](std::uint32_t descriptor, std::uint32_t event) {
        inject_wifi_driver_event(descriptor, event);
      });
  configure_darwin_notify_state();
  shared_state_->darwin_kernel_identity =
      make_darwin_kernel_identity_profile(rootfs_);
  shared_state_->device_product_type = device_profile_.product_type;
  shared_state_->device_board_config = device_profile_.board_config;
  shared_state_->device_hardware_model = device_profile_.hardware_model;
  shared_state_->device_model_number = device_profile_.model_number;
  shared_state_->device_ram_bytes = device_profile_.ram_bytes;
  shared_state_->device_cpu_type = arm_mach_cpu_type;
  shared_state_->graphics_accelerator = device_profile_.graphics_accelerator;
  shared_state_->graphics_driver_bundle =
      std::string{device_profile_.graphics_driver_bundle};
  shared_state_->device_cpu_subtype = mach_cpu_subtype_for_architecture(
      arm_architecture_for_model(device_profile_.cpu_model));
  shared_state_->baseband_device_state.set_available(
      device_profile_.baseband_transport !=
      BasebandTransportProfile::Unavailable);
  shared_state_->baseband_device_state.set_transport_writable(
      device_profile_.baseband_transport != BasebandTransportProfile::Offline);
  shared_state_->baseband_device_state.set_mux_channel_capacity(
      device_profile_.baseband_transport == BasebandTransportProfile::Offline
          ? bsd::baseband_device::offline_mux_channel_capacity
          : 0U);
  shared_state_->mounts.clear();
  for (const auto &volume : hfs_volumes_.volumes()) {
    shared_state_->mounts.push_back({"hfs", volume.mount_point,
                                     volume.mounted_device,
                                     volume.mount_flags});
  }
  device_profile_.display = display_state_->geometry();
  shared_state_->display_geometry = device_profile_.display;
  shared_state_->user_interface_geometry = device_profile_.user_interface;
  core_surface_hle_.set_shared_state(shared_state_);
  core_surface_hle_.set_scene_coordinator(scene_coordinator_);
  core_surface_hle_.set_surface_port_handlers(
      [this](std::uint32_t process_id, std::uint32_t surface_id) {
        std::lock_guard lock{shared_state_->mach_mutex};
        return mach_support::create_surface_transport_send_right_locked(
            *shared_state_, *surface_store_, process_id, surface_id);
      },
      [this](std::uint32_t process_id,
             std::uint32_t port_name) -> std::optional<std::uint32_t> {
        std::lock_guard lock{shared_state_->mach_mutex};
        return mach_support::resolve_surface_transport_locked(
            *shared_state_, process_id, port_name);
      });
  opengles_hle_.set_shared_state(shared_state_);
  opengles_hle_.set_scene_coordinator(scene_coordinator_);
  mbx2d_hle_.set_shared_state(shared_state_);
  mobile_framebuffer_hle_.set_shared_state(shared_state_);
  mobile_framebuffer_hle_.set_scene_coordinator(scene_coordinator_);
  mobile_framebuffer_hle_.set_frame_presented_handler(
      [this](std::uint32_t process_id) {
        graphics_services_input::complete_home_transition_after_present(
            *shared_state_, process_id, scene_coordinator_.get());
      });
  register_core_telephony_hle(
      userland_hle_, [this] { return wifi_state_; },
      [this](const WifiSnapshot &before, const WifiSnapshot &after) {
        apply_wifi_transition(before, after);
        apple80211_hle_.publish_state_change(before, after);
      },
      device_profile_.baseband_transport !=
          BasebandTransportProfile::Virtual);
  register_dns_configuration_hle(userland_hle_);
  register_app_support_hle(userland_hle_);
  register_lockdown_hle(userland_hle_, activated, lockdown_profile);
  register_bluetooth_manager_hle(userland_hle_);
  register_mbx_connect_hle(userland_hle_);
  graphics_services_input::register_springboard_alert_observers(
      userland_hle_,
      [this](
          std::uint32_t object,
          graphics_services_input::SpringBoardAlertObservation observation) {
        graphics_services_input::record_springboard_alert_state(
            *shared_state_, object, observation);
      });
  graphics_services_input::register_springboard_lock_observer(
      userland_hle_, [this](bool locked) {
        graphics_services_input::record_springboard_lock_state(
            *shared_state_, locked);
      });
  graphics_services_input::register_application_suspension_observer(
      userland_hle_, [this](std::uint32_t process_id, bool suspended) {
        graphics_services_input::record_application_suspension_state(
            *shared_state_, process_id, suspended,
            scene_coordinator_.get());
      });
  graphics_services_input::register_springboard_application_handoff_animation(
      userland_hle_, [this] {
        return graphics_services_input::
            take_pending_application_handoff_animation(*shared_state_);
      });
  layerkit_hle_.register_handlers(userland_hle_, shared_state_,
                                  scene_coordinator_, output_);
  thread_ports_.emplace(0, process_.thread_port);
  if (!mach_task_identity::initialize_root(*shared_state_, process_)) {
    throw std::runtime_error{"failed to initialize root Mach task identity"};
  }
  shared_state_->processes[process_.pid] =
      KernelSharedState::ProcessRecord{process_.parent_pid,
                                       process_.process_group,
                                       process_.uid,
                                       process_.effective_uid,
                                       process_.gid,
                                       process_.effective_gid,
                                       process_.exit_status,
                                       process_.termination_signal,
                                       process_.exited,
                                       "launchd",
                                       "/sbin/launchd",
                                       {"/sbin/launchd"},
                                       {"PATH=/usr/bin:/bin:/usr/sbin:/sbin",
                                        "HOME=/var/root", "SHELL=/bin/sh"},
                                       KernelSharedState::GraphicsInputAbi::
                                           Darwin9_0,
                                       {},
                                       {}};
  install_commpage();
}

void CompatibilityKernel::configure_darwin_notify_state() {
  const auto provider = [state = ringer_switch_state_] {
    return static_cast<std::uint64_t>(state->active());
  };
  darwin_notify_state_hle_.set_provider(
      std::string{ringer_switch_notification_name}, provider);
  darwin_notify_state_hle_.set_provider(
      std::string{springboard_ringer_switch_notification_name}, provider);
  darwin_notify_state_hle_.set_notification_dispatcher(
      [weak_state = std::weak_ptr<KernelSharedState>{shared_state_}](
          std::uint32_t process_id, std::uint32_t port_name,
          std::uint32_t token) {
        const auto state = weak_state.lock();
        if (!state)
          return;
        std::lock_guard lock{state->mach_mutex};
        const auto destination =
            state->mach_namespaces.resolve(process_id, port_name);
        if (!destination ||
            !state->mach_port_objects.contains(*destination)) {
          return;
        }
        KernelSharedState::MachMessage message;
        message.bytes.resize(darwin::mig_wire::message_header_size,
                             std::byte{0});
        const auto write_word = [&message](std::size_t offset,
                                           std::uint32_t value) {
          for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
            message.bytes[offset + byte] =
                static_cast<std::byte>(value >> (byte * 8U));
          }
        };
        write_word(darwin::mig_wire::header_bits_offset, 19U);
        write_word(
            darwin::mig_wire::header_size_offset,
            static_cast<std::uint32_t>(message.bytes.size()));
        write_word(darwin::mig_wire::header_remote_port_offset,
                   *destination);
        write_word(darwin::mig_wire::header_identifier_offset, token);
        message.destination = *destination;
        message.sender_pid = 0;
        state->enqueue_mach_message_locked(*destination, std::move(message));
      });
}

bool CompatibilityKernel::set_virtual_processor_count(
    std::size_t processor_count) {
  if (processor_count == 0 ||
      processor_count > std::numeric_limits<std::uint8_t>::max()) {
    return false;
  }
  virtual_processor_count_ = static_cast<std::uint32_t>(processor_count);
  install_commpage();
  return true;
}

void CompatibilityKernel::enqueue_baseband_input(
    std::span<const std::byte> bytes) {
  shared_state_->baseband_device_state.enqueue_receive(bytes);
}

void CompatibilityKernel::enqueue_touch_input(const TouchInput &input) {
  const PerformanceLatencyScope latency{PerfLatencyKind::InputEnqueue};
  performance_counters().discard_pending_vsync_callbacks();
  bool home_recovery_requested = false;
  const auto result =
      graphics_services_input::enqueue_touch(*shared_state_, input,
                                             scene_coordinator_.get(),
                                             presentation_tracker_.get(),
                                             &home_recovery_requested);
  const auto phase = [phase = input.phase] {
    switch (phase) {
    case TouchPhase::Down:
      return "down";
    case TouchPhase::Move:
      return "move";
    case TouchPhase::Up:
      return "up";
    case TouchPhase::Cancel:
      return "cancel";
    }
    return "unknown";
  }();
  output_.write("[input] touch phase=" + std::string{phase} + " x=" +
                std::to_string(input.x) + " y=" + std::to_string(input.y) +
                (result == graphics_services_input::EnqueueResult::Queued
                     ? " queued\n"
                     : " deferred\n"));
  if (home_recovery_requested) {
    output_.line(
        "[input] recovery=interrupted-lock-launch action=home");
    enqueue_system_button_impl(
        SystemButtonInput{SystemButton::Home, SystemButtonPhase::Down}, true);
    enqueue_system_button_impl(
        SystemButtonInput{SystemButton::Home, SystemButtonPhase::Up}, true);
  }
}

void CompatibilityKernel::enqueue_system_button(
    const SystemButtonInput &input) {
  enqueue_system_button_impl(input, false);
}

void CompatibilityKernel::enqueue_system_button_impl(
    const SystemButtonInput &input, bool force_home_transition) {
  const PerformanceLatencyScope latency{PerfLatencyKind::InputEnqueue};
  performance_counters().discard_pending_vsync_callbacks();
  bool wake_button_pressed_while_display_asleep = false;
  bool wake_only_system_button = false;
  bool begins_display_lock_transaction = false;
  if (input.phase == SystemButtonPhase::Down &&
      (input.button == SystemButton::Home ||
       input.button == SystemButton::Lock)) {
    std::lock_guard mach_lock{shared_state_->mach_mutex};
    const auto display_asleep =
        shared_state_->requested_display_power_state.value_or(1U) == 0U;
    if (display_asleep && !force_home_transition) {
      wake_button_pressed_while_display_asleep = true;
      wake_only_system_button = true;
      shared_state_->host_display_intent =
          KernelSharedState::HostDisplayIntent::WakePending;
      shared_state_->host_display_wake_after_lock_sequence =
          shared_state_->host_display_current_lock_down_sequence;
      shared_state_->host_display_wake_power_on_acknowledged = false;
      // Both physical Home and Sleep/Wake illuminate the panel before
      // SpringBoard has finished rebuilding the lock scene. Preserve the
      // original guest-visible event; only the host panel transition is
      // synthesized here.
      shared_state_->host_display_hardware_wake_pending = true;
      shared_state_->requested_display_power_state = 1U;
    } else if (input.button == SystemButton::Lock &&
               !force_home_transition) {
      // Publish a pending Lock transaction before the GSEvent becomes
      // visible. Do not force the panel off here: SpringBoard owns the
      // long-press timer and its native SBPowerDownController must remain
      // visible until the guest asks IOKit to power down.
      shared_state_->host_display_intent =
          KernelSharedState::HostDisplayIntent::LockPending;
      shared_state_->host_display_wake_power_on_acknowledged = false;
      shared_state_->host_display_hardware_wake_pending = false;
      begins_display_lock_transaction = true;
    } else if (!force_home_transition &&
               input.button == SystemButton::Home &&
               (shared_state_->springboard_unlock_touch_pending ||
                shared_state_->springboard_unlock_touch_active)) {
      // The LCD can already be on while SpringBoard still owns the lock
      // scene. Home is then another wake notification, not an App-exit
      // barrier.
      wake_only_system_button = true;
    }
  }
  if (wake_button_pressed_while_display_asleep) {
    graphics_services_input::record_lock_wake_request(*shared_state_);
  }
  std::uint64_t system_input_sequence = 0;
  const auto result = graphics_services_input::enqueue_system_button(
      *shared_state_, input, &system_input_sequence,
      begins_display_lock_transaction);
  // A sleeping Home or Sleep/Wake button is a wake request for SpringBoard,
  // not a new suspend transition. The firmware prepares its lock scene and
  // then requests LCD power through IOKit.
  if ((input.button == SystemButton::Home ||
       input.button == SystemButton::Lock) &&
      input.phase == SystemButtonPhase::Down &&
      (!wake_only_system_button || force_home_transition)) {
    graphics_services_input::suspend_active_application(
        *shared_state_,
        input.button == SystemButton::Lock
            ? KernelSharedState::ApplicationSuspensionReason::Lock
            : KernelSharedState::ApplicationSuspensionReason::Home,
        scene_coordinator_.get(), system_input_sequence);
  }
  const auto button = [value = input.button] {
    switch (value) {
    case SystemButton::Home:
      return "home";
    case SystemButton::Lock:
      return "lock";
    case SystemButton::VolumeUp:
      return "volume-up";
    case SystemButton::VolumeDown:
      return "volume-down";
    }
    return "unknown";
  }();
  output_.write("[input] button=" + std::string{button} + " phase=" +
                (input.phase == SystemButtonPhase::Down ? "down" : "up") +
                (result == graphics_services_input::EnqueueResult::Queued
                     ? " queued\n"
                     : " deferred\n"));
}

void CompatibilityKernel::set_ringer_switch_active(bool active) {
  if (!ringer_switch_state_->set_active(active))
    return;
  performance_counters().discard_pending_vsync_callbacks();
  darwin_notify_state_hle_.publish(ringer_switch_notification_name);
  darwin_notify_state_hle_.publish(
      springboard_ringer_switch_notification_name);
  const auto result =
      graphics_services_input::enqueue_ringer_switch_change(*shared_state_,
                                                            active);
  output_.write(
      "[input] ringer-switch=" +
      std::string{active ? "ring" : "silent"} +
      (result == graphics_services_input::EnqueueResult::Queued
           ? " queued\n"
           : " deferred\n"));
}

void CompatibilityKernel::toggle_ringer_switch() {
  const auto active = ringer_switch_state_->toggle();
  performance_counters().discard_pending_vsync_callbacks();
  darwin_notify_state_hle_.publish(ringer_switch_notification_name);
  darwin_notify_state_hle_.publish(
      springboard_ringer_switch_notification_name);
  const auto result =
      graphics_services_input::enqueue_ringer_switch_change(*shared_state_,
                                                            active);
  output_.write(
      "[input] ringer-switch=" +
      std::string{active ? "ring" : "silent"} +
      (result == graphics_services_input::EnqueueResult::Queued
           ? " queued\n"
           : " deferred\n"));
}

bool CompatibilityKernel::display_powered_on() const {
  std::lock_guard mach_lock{shared_state_->mach_mutex};
  return shared_state_->requested_display_power_state.value_or(1U) != 0;
}

std::vector<std::byte> CompatibilityKernel::take_baseband_output() {
  return shared_state_->baseband_device_state.take_transmitted();
}

bool CompatibilityKernel::refresh_display_scanout() {
  bool power_on = false;
  {
    std::lock_guard mach_lock{shared_state_->mach_mutex};
    power_on =
        shared_state_->requested_display_power_state.value_or(1U) != 0U;
  }

  if (!power_on) {
    display_state_->set_powered_on(false);
    return false;
  }

  const auto waking = !display_state_->powered_on();
  // Once SpringBoard uses transactional MobileFramebuffer layers, SwapEnd is
  // the sole frame boundary. Polling the legacy default surface here would
  // expose partially rendered offscreen buffers between layer transactions.
  if (mobile_framebuffer_hle_.has_active_layers()) {
    display_state_->set_powered_on(true);
    return false;
  }

  // Stage the legacy backing while the panel is still dark, then reveal the
  // retained scanout.  Reversing this order exposes the pre-wake App or a
  // partially cleared SpringBoard buffer for one host frame.
  if (waking) {
    const auto refreshed =
        core_surface_hle_.refresh_default_scanout(memory_, process_.pid);
    display_state_->set_powered_on(true);
    if (refreshed) {
      output_.write(
          "[display] scanout pid=" + std::to_string(process_.pid) +
          " frame=" +
          std::to_string(display_state_->presented_frames()) + "\n");
    }
    return refreshed;
  }

  display_state_->set_powered_on(true);
  const auto now = shared_state_->clock.now();
  if (next_display_scanout_deadline_ && now < *next_display_scanout_deadline_) {
    return false;
  }
  next_display_scanout_deadline_ =
      now + iokit_abi::display_vsync::period_absolute_time;
  const auto refreshed =
      core_surface_hle_.refresh_default_scanout(memory_, process_.pid);
  if (refreshed) {
    output_.write(
        "[display] scanout pid=" + std::to_string(process_.pid) +
        " frame=" + std::to_string(display_state_->presented_frames()) + "\n");
  }
  return refreshed;
}

bool CompatibilityKernel::owns_display_scanout() const {
  const auto backing =
      surface_store_->find(iokit_abi::apple_h1clcd_default_surface_id);
  return backing &&
         backing->provenance.producer_process_id == process_.pid;
}

void CompatibilityKernel::install_commpage() {
  // The iPhoneOS 1.0 libSystem shipped in this firmware directly reads the
  // legacy ARM commpage at 0x40000000 (for example, time data at +0x40).
  // Later ARM XNU moved the page to 0xffff4000, so xnu-4903's address must
  // not be projected backwards onto this 2007 user ABI.
  constexpr std::uint32_t commpage_address = 0x40000000U;
  if (!memory_.mapped(commpage_address)) {
    static_cast<void>(memory_.map(commpage_address, AddressSpace::page_size,
                                  MemoryPermission::Read));
  }
  std::array<std::byte, AddressSpace::page_size> commpage{};
  constexpr std::string_view signature{"commpage 32-bit"};
  for (std::size_t index = 0; index < signature.size(); ++index) {
    commpage[index] = static_cast<std::byte>(signature[index]);
  }
  commpage[0x1e] = std::byte{1}; // commpage format version
  commpage[0x22] = static_cast<std::byte>(virtual_processor_count_);
  memory_.copy_in(commpage_address, commpage);
}

void CompatibilityKernel::prepare_exec(std::size_t processor_id) {
  release_close_on_exec_descriptors();
  install_commpage();
  userland_hle_.reset_mappings();
  darwin_notify_state_hle_.reset();
  core_audio_hle_.reset();
  userland_hle_.record_loaded_image(process_image_);
  apple80211_hle_.reset(process_.pid);
  core_surface_hle_.reset();
  opengles_hle_.reset();
  mbx2d_hle_.reset();
  mobile_framebuffer_hle_.reset();
  layerkit_hle_.reset();
  next_display_scanout_deadline_.reset();
  signal_actions_ = {};
  signal_mask_ = 0;
  process_.waiting_for_events = false;
  const auto current_thread_port =
      thread_ports_.find(processor_id) != thread_ports_.end()
          ? thread_ports_.at(processor_id)
          : process_.thread_port;
  std::optional<std::uint32_t> surviving_thread_object;
  std::optional<std::uint32_t> surviving_thread_policy;
  {
    std::lock_guard mach_lock{shared_state_->mach_mutex};
    auto &thread_objects =
        shared_state_->task_thread_port_objects[process_.pid];
    if (const auto current = thread_objects.find(
            static_cast<std::uint32_t>(processor_id));
        current != thread_objects.end()) {
      surviving_thread_object = current->second;
    } else {
      surviving_thread_object = shared_state_->mach_namespaces.resolve(
          process_.pid, current_thread_port);
    }
    if (surviving_thread_object) {
      if (const auto policy = process_.thread_disk_io_policies.find(
              *surviving_thread_object);
          policy != process_.thread_disk_io_policies.end()) {
        surviving_thread_policy = policy->second;
      }
    }
    thread_objects.clear();
    if (surviving_thread_object) {
      thread_objects[static_cast<std::uint32_t>(processor_id)] =
          *surviving_thread_object;
    }
  }
  process_.thread_disk_io_policies.clear();
  if (surviving_thread_object && surviving_thread_policy) {
    process_.thread_disk_io_policies.emplace(*surviving_thread_object,
                                             *surviving_thread_policy);
  }
  thread_ports_.clear();
  thread_ports_.emplace(processor_id, current_thread_port);
  disabled_thread_signals_.clear();
  pending_waits_.clear();
  pending_mach_receives_.clear();
  pending_kevents_.clear();
  pending_recvmsgs_.clear();
  pending_socket_reads_.clear();
  pending_host_connects_.clear();
  pending_host_accepts_.clear();
  pending_host_writes_.clear();
  pending_baseband_writes_.clear();
  pending_unix_accepts_.clear();
  pending_flocks_.clear();
  pending_record_locks_.clear();
  pending_selects_.clear();
  pending_timers_.clear();
  pending_semaphore_waits_.clear();
  aio_completions_.clear();
}

std::size_t CompatibilityKernel::install_mapped_user_image(
    Cpu &cpu, const std::filesystem::path &image_path,
    std::uint32_t mapping_address, std::uint32_t mapping_size,
    std::uint64_t file_offset, bool shared_cache_mapping) {
  // A dyld shared-cache mapping is backed by a container/subcache rather than
  // by a standalone Mach-O file. Its image header and linkedit data are
  // distributed across cache mappings, so MachOImage::parse(path) cannot be
  // used here. HLE patching also must not write into shared cache __TEXT;
  // cache targets are resolved by the translator/dispatch path instead.
  const auto installed = shared_cache_mapping
                             ? 0U
                             : userland_hle_.install_mapped_image(
                                   cpu, process_.pid, image_path,
                                   mapping_address, mapping_size, file_offset,
                                   arm_architecture_for_model(
                                       device_profile_.cpu_model));
  if (!shared_cache_mapping && mapped_executable_handler_) {
    mapped_executable_handler_(image_path, mapping_address, mapping_size,
                               file_offset);
  }
  constexpr std::string_view uikit_image{"/UIKit.framework/UIKit"};
  constexpr std::string_view graphics_services_image{
      "/GraphicsServices.framework/GraphicsServices"};
  constexpr std::string_view quartz_core_image{
      "/QuartzCore.framework/QuartzCore"};
  const auto path = image_path.generic_string();
  if (shared_cache_mapping) return installed;
  if (path.ends_with(uikit_image)) {
    std::lock_guard mach_lock{shared_state_->mach_mutex};
    if (const auto process = shared_state_->processes.find(process_.pid);
        process != shared_state_->processes.end() &&
        process->second.graphics_input_abi ==
            KernelSharedState::GraphicsInputAbi::LegacyMouse) {
      process->second.graphics_input_abi =
          KernelSharedState::GraphicsInputAbi::Darwin9_0;
    }
  } else if (path.ends_with(graphics_services_image)) {
    const auto profile =
        GraphicsServicesInputProfile::detect(MachOImage::parse(
            image_path, arm_architecture_for_model(device_profile_.cpu_model)));
    if (profile) {
      std::lock_guard mach_lock{shared_state_->mach_mutex};
      if (const auto process = shared_state_->processes.find(process_.pid);
          process != shared_state_->processes.end() &&
          process->second.graphics_input_abi != *profile) {
        process->second.graphics_input_abi = *profile;
        output_.write(
            "[input] GraphicsServices touch ABI profile=" +
            std::string{GraphicsServicesInputProfile::for_abi(*profile).name} +
            " pid=" + std::to_string(process_.pid) + "\n");
      }
    }
  } else if (path.ends_with(quartz_core_image)) {
    const auto profile =
        CoreAnimationRemoteProfile::detect(MachOImage::parse(
            image_path, arm_architecture_for_model(device_profile_.cpu_model)));
    if (profile) {
      std::lock_guard mach_lock{shared_state_->mach_mutex};
      if (const auto process = shared_state_->processes.find(process_.pid);
          process != shared_state_->processes.end() &&
          process->second.core_animation_remote_profile != profile) {
        process->second.core_animation_remote_profile = *profile;
        output_.write(
            "[scene] CoreAnimation remote profile=" +
            std::string{profile->name} + " pid=" +
            std::to_string(process_.pid) + "\n");
      }
    }
  }
  return installed;
}

void CompatibilityKernel::install_main_image_hle(
    Cpu &cpu, std::string_view mapped_guest_path) {
  auto relative = std::filesystem::path{
      mapped_guest_path.empty() ? std::string_view{process_image_}
                                : mapped_guest_path};
  if (relative.is_absolute())
    relative = relative.relative_path();
  const auto host_path = rootfs_ / relative;
  const auto image = MachOImage::parse(
      host_path, arm_architecture_for_model(device_profile_.cpu_model));
  for (const auto &segment : image.segments()) {
    if (segment.file_size == 0)
      continue;
    static_cast<void>(install_mapped_user_image(
        cpu, host_path, segment.vm_address, segment.file_size,
        segment.file_offset));
  }
}

void CompatibilityKernel::set_process_image(
    std::string_view guest_path,
    std::span<const std::byte> code_signature_entitlements) {
  process_image_ = guest_path;
  auto name = std::filesystem::path{guest_path}.filename().string();
  if (name.empty())
    name = "unknown";
  if (name.size() > 16)
    name.resize(16);
  auto &record = shared_state_->processes[process_.pid];
  record.parent_pid = process_.parent_pid;
  record.process_group = process_.process_group;
  record.uid = process_.uid;
  record.effective_uid = process_.effective_uid;
  record.gid = process_.gid;
  record.effective_gid = process_.effective_gid;
  record.exited = false;
  record.exit_status = 0;
  record.termination_signal = 0;
  record.command = std::move(name);
  record.executable_path = std::string{guest_path};
  record.code_signature_entitlements.assign(
      code_signature_entitlements.begin(), code_signature_entitlements.end());
  record.display_orientation = detect_application_display_orientation(
      rootfs_, guest_path);
  if (record.display_orientation != DisplayOrientation::Portrait) {
    output_.write("[display] application orientation=" +
                  std::string{display_orientation_name(
                      record.display_orientation)} +
                  " pid=" + std::to_string(process_.pid) + "\n");
  }
  record.graphics_input_abi =
      KernelSharedState::GraphicsInputAbi::LegacyMouse;
  if (record.arguments.empty())
    record.arguments.push_back(record.executable_path);
  {
    std::lock_guard lock{shared_state_->mach_mutex};
    auto &events =
        shared_state_->process_kevent_states[process_.pid];
    ++events.exec_generation;
    if (events.exec_generation == 0U)
      events.exec_generation = 1U;
  }
}

void CompatibilityKernel::set_process_arguments(
    const std::vector<std::string> &arguments,
    const std::vector<std::string> &environment) {
  auto &record = shared_state_->processes[process_.pid];
  record.arguments = arguments;
  record.environment = environment;
}

bool CompatibilityKernel::complete_wait(Cpu &cpu, std::uint32_t child_pid,
                                        std::uint32_t wait_status) {
  const auto pending = pending_waits_.find(cpu.processor_id());
  if (pending == pending_waits_.end())
    return false;
  if (pending->second.status_address != 0 &&
      !memory_.write32(pending->second.status_address, wait_status)) {
    bsd_error(cpu, efault);
  } else {
    bsd_success(cpu, child_pid);
  }
  pending_waits_.erase(pending);
  process_.waiting_for_events = !pending_waits_.empty();
  output_.write("[process] reap parent=" + std::to_string(process_.pid) +
                " child=" + std::to_string(child_pid) + "\n");
  return true;
}

bool CompatibilityKernel::fail_wait(Cpu &cpu, std::uint32_t error) {
  const auto pending = pending_waits_.find(cpu.processor_id());
  if (pending == pending_waits_.end())
    return false;
  bsd_error(cpu, error);
  pending_waits_.erase(pending);
  process_.waiting_for_events = !pending_waits_.empty();
  return true;
}

std::optional<KernelSharedState::DescriptorTransfer>
CompatibilityKernel::export_descriptor(std::uint32_t fd) const {
  for (unsigned depth = 0; depth < 256; ++depth) {
    const auto duplicate = duplicated_descriptors_.find(fd);
    if (duplicate == duplicated_descriptors_.end())
      break;
    fd = duplicate->second;
  }
  if (const auto file = file_descriptors_.find(fd);
      file != file_descriptors_.end()) {
    KernelSharedState::DescriptorTransfer transfer;
    transfer.kind = KernelSharedState::DescriptorTransfer::Kind::File;
    transfer.file_path = file->second;
    transfer.file_offset =
        file_offsets_.contains(fd) ? file_offsets_.at(fd) : 0;
    transfer.file_status_flags =
        file_status_flags_.contains(fd) ? file_status_flags_.at(fd) : 0;
    if (const auto description = regular_file_open_descriptions_.find(fd);
        description != regular_file_open_descriptions_.end()) {
      transfer.regular_file_open_description = description->second;
    }
    if (const auto block = virtual_block_descriptors_.find(fd);
        block != virtual_block_descriptors_.end()) {
      transfer.block_device = block->second;
    }
    return transfer;
  }
  const auto virtual_descriptor = virtual_descriptors_.find(fd);
  if (virtual_descriptor == virtual_descriptors_.end())
    return std::nullopt;
  KernelSharedState::DescriptorTransfer transfer;
  transfer.kind = KernelSharedState::DescriptorTransfer::Kind::Virtual;
  transfer.virtual_type = virtual_descriptor->second;
  transfer.file_status_flags = file_status_flags_.contains(fd)
                                   ? file_status_flags_.at(fd)
                                   : darwin::open_flag::read_write;
  if (const auto endpoint = socket_pair_endpoints_.find(fd);
      endpoint != socket_pair_endpoints_.end()) {
    transfer.socket_endpoint = endpoint->second;
  }
  if (const auto listener = unix_listener_states_.find(fd);
      listener != unix_listener_states_.end()) {
    transfer.unix_listener_state = listener->second;
  }
  if (const auto state = route_socket_states_.find(fd);
      state != route_socket_states_.end()) {
    transfer.route_socket_state = state->second;
  }
  if (const auto socket = virtual_udp_sockets_.find(fd);
      socket != virtual_udp_sockets_.end()) {
    transfer.virtual_udp_socket = socket->second;
  }
  if (const auto bound = bound_socket_names_.find(fd);
      bound != bound_socket_names_.end()) {
    transfer.bound_name = bound->second;
  }
  transfer.listening = listening_sockets_.contains(fd);
  if (const auto queue = kqueues_.find(fd); queue != kqueues_.end()) {
    transfer.kqueue_registrations = queue->second;
  }
  return transfer;
}

std::optional<std::uint32_t> CompatibilityKernel::import_descriptor(
    const KernelSharedState::DescriptorTransfer &transfer) {
  const auto fd = allocate_file_descriptor();
  if (!fd)
    return std::nullopt;
  if (transfer.kind == KernelSharedState::DescriptorTransfer::Kind::File) {
    file_descriptors_[*fd] = transfer.file_path;
    file_offsets_[*fd] = transfer.file_offset;
    file_status_flags_[*fd] = transfer.file_status_flags;
    if (transfer.regular_file_open_description) {
      regular_file_open_descriptions_[*fd] =
          transfer.regular_file_open_description;
    } else {
      static_cast<void>(ensure_regular_file_open_description(*fd));
    }
    if (transfer.block_device) {
      virtual_block_descriptors_[*fd] = *transfer.block_device;
    }
  } else {
    virtual_descriptors_[*fd] = transfer.virtual_type;
    file_status_flags_[*fd] = transfer.file_status_flags;
    if (transfer.virtual_type == "route-socket") {
      std::lock_guard route_lock{shared_state_->route_socket_mutex};
      route_socket_states_[*fd] =
          transfer.route_socket_state
              ? transfer.route_socket_state
              : std::make_shared<KernelSharedState::RouteSocketState>(
                    KernelSharedState::RouteSocketState{
                        shared_state_->next_route_socket_identifier++,
                        shared_state_->next_route_message_identifier, 0});
    }
    if (transfer.socket_endpoint) {
      socket_pair_endpoints_[*fd] = *transfer.socket_endpoint;
    }
    if (transfer.virtual_udp_socket) {
      virtual_udp_sockets_[*fd] = transfer.virtual_udp_socket;
    }
    if (transfer.unix_listener_state) {
      unix_listener_states_[*fd] = transfer.unix_listener_state;
    }
    if (!transfer.bound_name.empty()) {
      bound_socket_names_[*fd] = transfer.bound_name;
    }
    if (transfer.listening)
      listening_sockets_.insert(*fd);
    if (!transfer.kqueue_registrations.empty() ||
        transfer.virtual_type == "kqueue") {
      kqueues_[*fd] = transfer.kqueue_registrations;
    }
  }
  // SCM_RIGHTS never propagates FD_CLOEXEC to the newly installed fd.
  descriptor_flags_[*fd] = 0;
  return fd;
}

bool CompatibilityKernel::deliver_pending_io(Cpu &cpu) {
  std::lock_guard lock{mutex_};
  return deliver_pending_io_locked(cpu);
}

bool CompatibilityKernel::deliver_pending_event(Cpu &cpu) {
  std::lock_guard lock{mutex_};
  if (pending_mach_receives_.contains(cpu.processor_id()))
    return deliver_pending_mach_if_ready_locked(cpu);
  return deliver_pending_io_locked(cpu);
}

bool CompatibilityKernel::deliver_pending_io_locked(Cpu &cpu) {
  for (auto event = scheduled_wifi_driver_events_.begin();
       event != scheduled_wifi_driver_events_.end();) {
    if (shared_state_->clock.now() < event->first) break;
    inject_wifi_driver_event(0, event->second);
    event = scheduled_wifi_driver_events_.erase(event);
  }
  if (const auto pending = pending_record_locks_.find(cpu.processor_id());
      pending != pending_record_locks_.end()) {
    if (!shared_state_->advisory_file_locks->try_set_record_lock(
            pending->second.permanent_file_id, process_.pid,
            pending->second.range)) {
      return false;
    }
    output_.write("[vfs] fcntl lock wake pid=" + std::to_string(process_.pid) +
                  " fd=" + std::to_string(pending->second.fd) + "\n");
    bsd_success(cpu, 0);
    pending_record_locks_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }
  if (const auto pending = pending_flocks_.find(cpu.processor_id());
      pending != pending_flocks_.end()) {
    if (!shared_state_->advisory_file_locks->try_acquire(
            *pending->second.description, pending->second.kind)) {
      return false;
    }
    output_.write("[vfs] flock wake pid=" + std::to_string(process_.pid) +
                  " fd=" + std::to_string(pending->second.fd) + "\n");
    bsd_success(cpu, 0);
    pending_flocks_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }
  if (const auto pending = pending_host_connects_.find(cpu.processor_id());
      pending != pending_host_connects_.end()) {
    const auto host = host_sockets_.find(pending->second.fd);
    if (host == host_sockets_.end()) {
      bsd_error(cpu, ebadf);
    } else {
      if (!host->second->writable())
        return false;
      const auto completed = host->second->finish_connect();
      if (completed.status == HostSocketStatus::WouldBlock)
        return false;
      if (completed.status == HostSocketStatus::Error) {
        bsd_error(cpu, completed.darwin_error);
      } else {
        bsd_success(cpu, 0);
      }
    }
    pending_host_connects_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }
  if (const auto pending = pending_host_accepts_.find(cpu.processor_id());
      pending != pending_host_accepts_.end()) {
    const auto host = host_sockets_.find(pending->second.fd);
    if (host == host_sockets_.end()) {
      bsd_error(cpu, ebadf);
    } else {
      const auto accepted = host->second->accept();
      if (accepted.status == HostSocketStatus::WouldBlock)
        return false;
      if (accepted.status == HostSocketStatus::Error) {
        bsd_error(cpu, accepted.darwin_error);
      } else if (const auto fd =
                     install_host_socket(accepted.accepted_socket)) {
        if (!copy_socket_address(pending->second.address,
                                 pending->second.length_address,
                                 accepted.address)) {
          host_sockets_.erase(*fd);
          virtual_descriptors_.erase(*fd);
          file_status_flags_.erase(*fd);
          descriptor_flags_.erase(*fd);
          bsd_error(cpu, efault);
        } else {
          bsd_success(cpu, *fd);
        }
      } else {
        bsd_error(cpu, 24); // EMFILE
      }
    }
    pending_host_accepts_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }
  if (const auto pending = pending_host_writes_.find(cpu.processor_id());
      pending != pending_host_writes_.end()) {
    if (!pending->second.socket) {
      bsd_error(cpu, ebadf);
    } else {
      const auto sent = pending->second.socket->send(
          pending->second.bytes, pending->second.destination);
      if (sent.status == HostSocketStatus::WouldBlock)
        return false;
      if (sent.status == HostSocketStatus::Error) {
        bsd_error(cpu, sent.darwin_error);
      } else {
        bsd_success(cpu, static_cast<std::uint32_t>(sent.transferred));
        output_.write("[network] write wake pid=" +
                      std::to_string(process_.pid) + " fd=" +
                      std::to_string(pending->second.fd) + " bytes=" +
                      std::to_string(sent.transferred) + "\n");
      }
    }
    pending_host_writes_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }
  if (const auto pending = pending_baseband_writes_.find(cpu.processor_id());
      pending != pending_baseband_writes_.end()) {
    if (!shared_state_->baseband_device_state.transport_writable())
      return false;
    const auto descriptor = virtual_descriptors_.find(pending->second.fd);
    if (descriptor == virtual_descriptors_.end() ||
        descriptor->second != bsd::baseband_device::descriptor_kind) {
      bsd_error(cpu, ebadf);
    } else {
      const auto written = shared_state_->baseband_device_state.write(
          pending->second.bytes);
      bsd_success(cpu, static_cast<std::uint32_t>(written));
      output_.write("[baseband] write wake pid=" +
                    std::to_string(process_.pid) + " fd=" +
                    std::to_string(pending->second.fd) + " bytes=" +
                    std::to_string(written) + "\n");
    }
    pending_baseband_writes_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }
  if (const auto pending = pending_unix_accepts_.find(cpu.processor_id());
      pending != pending_unix_accepts_.end()) {
    if (!complete_unix_accept(cpu, pending->second.fd, pending->second.address,
                              pending->second.length_address)) {
      return false;
    }
    pending_unix_accepts_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }
  if (const auto pending = pending_semaphore_waits_.find(cpu.processor_id());
      pending != pending_semaphore_waits_.end()) {
    const auto waiter =
        std::pair{process_.pid, static_cast<std::uint32_t>(cpu.processor_id())};
    bool awakened = false;
    bool terminated = false;
    bool timed_out = false;
    {
      std::lock_guard mach_lock{shared_state_->mach_mutex};
      terminated = shared_state_->semaphore_terminations.erase(waiter) != 0;
      awakened = !terminated &&
                 shared_state_->semaphore_wakeups.erase(waiter) != 0;
      timed_out = !terminated && !awakened && pending->second.deadline &&
                  shared_state_->clock.now() >= *pending->second.deadline;
      if (timed_out) {
        if (auto semaphore =
                shared_state_->mach_semaphores.find(pending->second.semaphore);
            semaphore != shared_state_->mach_semaphores.end()) {
          std::erase(semaphore->second.waiters, waiter);
        }
      }
    }
    if (!terminated && !awakened && !timed_out)
      return false;
    if (pending->second.bsd_result) {
      if (terminated)
        bsd_error(cpu, darwin::error::invalid_argument);
      else if (timed_out)
        bsd_error(cpu, 60); // ETIMEDOUT
      else
        bsd_success(cpu, 0);
    } else {
      cpu.registers()[0] =
          terminated ? darwin::mach::terminated
                     : timed_out ? darwin::mach::operation_timed_out
                                 : darwin::mach::success;
    }
    pending_semaphore_waits_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }
  if (const auto timer = pending_timers_.find(cpu.processor_id());
      timer != pending_timers_.end()) {
    bool bootstrap_ready = false;
    if (timer->second.bootstrap_retry) {
      std::lock_guard mach_lock{shared_state_->mach_mutex};
      const auto generation = shared_state_->bootstrap_service_generations.find(
          timer->second.bootstrap_retry->service_name);
      bootstrap_ready =
          generation != shared_state_->bootstrap_service_generations.end() &&
          generation->second >
              timer->second.bootstrap_retry->observed_generation;
    }
    if (shared_state_->clock.now() < timer->second.deadline &&
        !bootstrap_ready) {
      return false;
    }
    const auto pending = timer->second;
    pending_timers_.erase(timer);
    process_.waiting_for_events = false;
    if (bootstrap_ready) {
      output_.write("[timer] bootstrap-retry-ready pid=" +
                    std::to_string(process_.pid) + " service=" +
                    pending.bootstrap_retry->service_name + "\n");
    }
    if (pending.kind == PendingTimerKind::ClockSleep &&
        pending.wakeup_time_address) {
      const auto now = pending.calendar_clock ? shared_state_->clock.wall_time()
                                              : shared_state_->clock.now();
      const auto seconds = static_cast<std::uint32_t>(
          now / darwin::mach::clock::nanoseconds_per_second);
      const auto nanoseconds = static_cast<std::uint32_t>(
          now % darwin::mach::clock::nanoseconds_per_second);
      // XNU clock_sleep_trap deliberately ignores copyout's result.
      static_cast<void>(
          memory_.write32(*pending.wakeup_time_address +
                              darwin::mach::clock::timespec_seconds_offset,
                          seconds));
      static_cast<void>(
          memory_.write32(*pending.wakeup_time_address +
                              darwin::mach::clock::timespec_nanoseconds_offset,
                          nanoseconds));
    }
    cpu.registers()[0] = darwin::mach::success;
    cpu.clear_halt();
    return true;
  }
  if (const auto pending = pending_selects_.find(cpu.processor_id());
      pending != pending_selects_.end()) {
    std::uint32_t ready_count = 0;
    std::vector<std::uint32_t> ready_read_words(
        pending->second.read_words.size());
    std::vector<std::uint32_t> ready_write_words(
        pending->second.write_words.size());
    for (std::size_t word_index = 0;
         word_index < pending->second.read_words.size(); ++word_index) {
      for (std::uint32_t bit = 0; bit < 32; ++bit) {
        const auto fd = static_cast<std::uint32_t>(word_index * 32U + bit);
        if (fd >= pending->second.descriptor_count)
          continue;
        if ((pending->second.read_words[word_index] & (1U << bit)) != 0 &&
            descriptor_readable(fd)) {
          ready_read_words[word_index] |= 1U << bit;
          ++ready_count;
        }
        if ((pending->second.write_words[word_index] & (1U << bit)) != 0 &&
            descriptor_writable(fd)) {
          ready_write_words[word_index] |= 1U << bit;
          ++ready_count;
        }
      }
    }
    const auto timed_out =
        pending->second.deadline &&
        shared_state_->clock.now() >= *pending->second.deadline;
    if (ready_count == 0 && !timed_out)
      return false;
    bool copied = true;
    for (std::size_t index = 0; index < ready_read_words.size(); ++index) {
      if (pending->second.read_address != 0) {
        copied = copied &&
                 memory_.write32(pending->second.read_address +
                                     static_cast<std::uint32_t>(index * 4U),
                                 ready_read_words[index]);
      }
      if (pending->second.write_address != 0) {
        copied = copied &&
                 memory_.write32(pending->second.write_address +
                                     static_cast<std::uint32_t>(index * 4U),
                                 ready_write_words[index]);
      }
      if (pending->second.exception_address != 0) {
        copied = copied &&
                 memory_.write32(pending->second.exception_address +
                                     static_cast<std::uint32_t>(index * 4U),
                                 0);
      }
    }
    if (copied)
      bsd_success(cpu, ready_count);
    else
      bsd_error(cpu, efault);
    pending_selects_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }
  if (const auto pending = pending_socket_reads_.find(cpu.processor_id());
      pending != pending_socket_reads_.end()) {
    if (const auto descriptor =
            virtual_descriptors_.find(pending->second.fd);
        descriptor != virtual_descriptors_.end() &&
        descriptor->second == bsd::offline_serial_device::descriptor_kind) {
      if (auto bytes = offline_serial_state_.read(pending->second.size);
          !bytes.empty()) {
        if (!memory_.copy_in(pending->second.address, bytes)) {
          bsd_error(cpu, efault);
        } else {
          bsd_success(cpu, static_cast<std::uint32_t>(bytes.size()));
        }
        pending_socket_reads_.erase(pending);
        process_.waiting_for_events = false;
        cpu.clear_halt();
        return true;
      }
      if (!pending->second.deadline ||
          shared_state_->clock.now() < *pending->second.deadline) {
        return false;
      }
      bsd_success(cpu, 0);
      pending_socket_reads_.erase(pending);
      process_.waiting_for_events = false;
      cpu.clear_halt();
      return true;
    }
    if (!receive_socket_bytes(cpu, pending->second.fd, pending->second.address,
                              pending->second.size,
                              pending->second.source_address,
                              pending->second.source_length_address)) {
      return false;
    }
    pending_socket_reads_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }
  if (const auto pending = pending_recvmsgs_.find(cpu.processor_id());
      pending != pending_recvmsgs_.end()) {
    if (!receive_socket_message(cpu, pending->second.fd,
                                pending->second.message_address)) {
      return false;
    }
    pending_recvmsgs_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
  }
  const auto pending_kevent = pending_kevents_.find(cpu.processor_id());
  if (pending_kevent == pending_kevents_.end())
    return false;
  const auto ready = collect_ready_kevents(pending_kevent->second.queue_fd,
                                           pending_kevent->second.event_address,
                                           pending_kevent->second.event_count);
  if (!ready) {
    bsd_error(cpu, kqueues_.contains(pending_kevent->second.queue_fd) ? efault
                                                                      : ebadf);
  } else if (*ready != 0) {
    bsd_success(cpu, *ready);
  } else if (pending_kevent->second.deadline &&
             shared_state_->clock.now() >= *pending_kevent->second.deadline) {
    bsd_success(cpu, 0);
  } else {
    return false;
  }
  pending_kevents_.erase(pending_kevent);
  process_.waiting_for_events = false;
  cpu.clear_halt();
  return true;
}

std::string CompatibilityKernel::wait_reason(std::size_t processor) const {
  if (const auto pending = pending_flocks_.find(processor);
      pending != pending_flocks_.end()) {
    return "flock(fd=" + std::to_string(pending->second.fd) + ")";
  }
  if (const auto pending = pending_record_locks_.find(processor);
      pending != pending_record_locks_.end()) {
    return "fcntl-lock(fd=" + std::to_string(pending->second.fd) + ")";
  }
  if (const auto pending = pending_host_connects_.find(processor);
      pending != pending_host_connects_.end()) {
    return "connect(fd=" + std::to_string(pending->second.fd) + ")";
  }
  if (const auto pending = pending_host_accepts_.find(processor);
      pending != pending_host_accepts_.end()) {
    return "accept(fd=" + std::to_string(pending->second.fd) + ")";
  }
  if (const auto pending = pending_host_writes_.find(processor);
      pending != pending_host_writes_.end()) {
    return "write(fd=" + std::to_string(pending->second.fd) + ")";
  }
  if (const auto pending = pending_baseband_writes_.find(processor);
      pending != pending_baseband_writes_.end()) {
    return "write(baseband fd=" + std::to_string(pending->second.fd) + ")";
  }
  if (const auto pending = pending_unix_accepts_.find(processor);
      pending != pending_unix_accepts_.end()) {
    return "accept(fd=" + std::to_string(pending->second.fd) + ")";
  }
  if (const auto pending = pending_semaphore_waits_.find(processor);
      pending != pending_semaphore_waits_.end()) {
    return "semaphore(port=" + std::to_string(pending->second.semaphore) + ")";
  }
  if (const auto timer = pending_timers_.find(processor);
      timer != pending_timers_.end()) {
    const auto operation = [&] {
      switch (timer->second.kind) {
      case PendingTimerKind::MachWaitUntil:
        return "mach_wait_until";
      case PendingTimerKind::ThreadSwitch:
        return "thread_switch";
      case PendingTimerKind::ClockSleep:
        return "clock_sleep";
      }
      return "timer";
    }();
    return std::string{operation} +
           "(deadline=" + std::to_string(timer->second.deadline) + ")";
  }
  if (const auto pending = pending_selects_.find(processor);
      pending != pending_selects_.end()) {
    return "select(nfds=" + std::to_string(pending->second.descriptor_count) +
           ")";
  }
  if (const auto pending = pending_recvmsgs_.find(processor);
      pending != pending_recvmsgs_.end()) {
    return "recvmsg(fd=" + std::to_string(pending->second.fd) + ")";
  }
  if (const auto pending = pending_socket_reads_.find(processor);
      pending != pending_socket_reads_.end()) {
    return "read(fd=" + std::to_string(pending->second.fd) + ")";
  }
  if (const auto pending = pending_kevents_.find(processor);
      pending != pending_kevents_.end()) {
    const auto queue = kqueues_.find(pending->second.queue_fd);
    const auto registrations =
        queue == kqueues_.end() ? 0U : queue->second.size();
    return "kevent(fd=" + std::to_string(pending->second.queue_fd) +
           ",registrations=" + std::to_string(registrations) + ")";
  }
  if (const auto pending = pending_mach_receives_.find(processor);
      pending != pending_mach_receives_.end()) {
    std::string reason =
        "mach_msg(port=" + std::to_string(pending->second.receive_name);
    const std::lock_guard mach_lock{shared_state_->mach_mutex};
    if (const auto object = shared_state_->mach_namespaces.resolve(
            process_.pid, pending->second.receive_name)) {
      reason += ",object=" + std::to_string(*object);
      if (const auto members = shared_state_->mach_port_sets.find(*object);
          members != shared_state_->mach_port_sets.end()) {
        reason += ",members=" + std::to_string(members->second.size());
      }
    }
    return reason + ")";
  }
  if (const auto pending = pending_waits_.find(processor);
      pending != pending_waits_.end()) {
    return "wait4(target=" + std::to_string(pending->second.target_pid) + ")";
  }
  return process_.waiting_for_events ? "generic-event" : "none";
}

std::optional<std::uint64_t> CompatibilityKernel::next_timer_deadline() const {
  std::optional<std::uint64_t> deadline;
  for (const auto &[processor, timer] : pending_timers_) {
    static_cast<void>(processor);
    if (!deadline || timer.deadline < *deadline)
      deadline = timer.deadline;
  }
  for (const auto &[processor, wait] : pending_semaphore_waits_) {
    static_cast<void>(processor);
    if (wait.deadline && (!deadline || *wait.deadline < *deadline)) {
      deadline = wait.deadline;
    }
  }
  for (const auto &[processor, wait] : pending_kevents_) {
    static_cast<void>(processor);
    if (wait.deadline && (!deadline || *wait.deadline < *deadline)) {
      deadline = wait.deadline;
    }
  }
  for (const auto &[processor, wait] : pending_selects_) {
    static_cast<void>(processor);
    if (wait.deadline && (!deadline || *wait.deadline < *deadline)) {
      deadline = wait.deadline;
    }
  }
  for (const auto &[event_deadline, event] :
       scheduled_wifi_driver_events_) {
    static_cast<void>(event);
    if (!deadline || event_deadline < *deadline) {
      deadline = event_deadline;
    }
  }
  for (const auto &[processor, read] : pending_socket_reads_) {
    static_cast<void>(processor);
    if (read.deadline && (!deadline || *read.deadline < *deadline)) {
      deadline = read.deadline;
    }
  }
  for (const auto &[processor, receive] : pending_mach_receives_) {
    static_cast<void>(processor);
    if (receive.deadline && (!deadline || *receive.deadline < *deadline)) {
      deadline = receive.deadline;
    }
  }
  if (const auto audio = core_audio_hle_.next_io_proc_deadline();
      audio && (!deadline || *audio < *deadline)) {
    deadline = audio;
  }
  if (const auto interval_timer = kernel_bsd::interval_timer::next_deadline(
          *shared_state_, process_.pid);
      interval_timer && (!deadline || *interval_timer < *deadline)) {
    deadline = interval_timer;
  }
  {
    std::lock_guard mach_lock{shared_state_->mach_mutex};
    for (const auto &[port, timer] : shared_state_->mach_timers) {
      static_cast<void>(port);
      if (timer.deadline && (!deadline || *timer.deadline < *deadline)) {
        deadline = timer.deadline;
      }
    }
    if (const auto alarm = next_clock_alarm_deadline_locked(*shared_state_);
        alarm && (!deadline || *alarm < *deadline)) {
      deadline = alarm;
    }
    if (const auto vsync =
            kernel_iokit::display::next_vsync_deadline_locked(*shared_state_);
        vsync && (!deadline || *vsync < *deadline)) {
      deadline = vsync;
    }
    if (const auto capture =
            kernel_iokit::camera::next_capture_deadline_locked(*shared_state_);
        capture && (!deadline || *capture < *deadline)) {
      deadline = capture;
    }
  }
  return deadline;
}

std::optional<std::uint64_t>
CompatibilityKernel::next_display_vsync_deadline() const {
  std::lock_guard mach_lock{shared_state_->mach_mutex};
  return kernel_iokit::display::next_vsync_deadline_locked(*shared_state_);
}

void CompatibilityKernel::advance_absolute_time(std::uint64_t deadline) {
  shared_state_->clock.advance_to(deadline);
  {
    std::lock_guard mach_lock{shared_state_->mach_mutex};
    for (auto &[port, timer] : shared_state_->mach_timers) {
      if (!timer.deadline || *timer.deadline > deadline)
        continue;
      KernelSharedState::MachMessage message;
      message.bytes.resize(48);
      const auto put32 = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
          message.bytes[offset + byte] =
              static_cast<std::byte>(value >> (byte * 8U));
        }
      };
      put32(0, 19); // MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0)
      put32(4, static_cast<std::uint32_t>(message.bytes.size()));
      put32(8, port);
      put32(12, 0);
      put32(16, 0);
      put32(20, 0);
      message.destination = port;
      shared_state_->enqueue_mach_message_locked(port, std::move(message));
      timer.deadline.reset();
      output_.write("[timer] expired port=" + std::to_string(port) + "\n");
    }
    deliver_due_clock_alarms_locked(*shared_state_, deadline);
    kernel_iokit::display::deliver_due_vsync_locked(*shared_state_, deadline);
  }
  service_time_dependent_devices(deadline);
}

void CompatibilityKernel::service_time_dependent_devices(
    std::uint64_t deadline) {
  kernel_iokit::camera::service_due_captures(
      *shared_state_, process_.pid, memory_, *surface_store_, deadline);
  if (kernel_bsd::interval_timer::service_due(*shared_state_, process_.pid,
                                               deadline)) {
    static_cast<void>(
        deliver_signal(kernel_bsd::interval_timer::expiration_signal));
  }
  schedule_due_audio_io(deadline);
}

void CompatibilityKernel::schedule_due_audio_io(std::uint64_t deadline) {
  auto callback = core_audio_hle_.take_due_io_proc(deadline);
  if (!callback)
    return;
  if (callback->process_id != process_.pid || !thread_create_handler_) {
    core_audio_hle_.io_proc_schedule_failed(callback->process_id,
                                            callback->native,
                                            callback->io_proc_id);
    return;
  }
  const auto update_thread_pointer = [&](std::size_t processor) {
    if (!thread_pointer_update_handler_)
      return !callback->cthread_self.has_value();
    return thread_pointer_update_handler_(
        process_.pid, static_cast<std::uint32_t>(processor),
        callback->cthread_self);
  };
  if (callback->processor) {
    darwin::arm_thread::GeneralState state{};
    std::copy(callback->registers.begin(), callback->registers.end(),
              state.begin());
    state[darwin::arm_thread::cpsr_index] = callback->cpsr;
    const auto slot = static_cast<std::uint32_t>(*callback->processor);
    if (thread_state_update_handler_ && thread_wake_handler_ &&
        update_thread_pointer(*callback->processor) &&
        thread_state_update_handler_(process_.pid, slot, state) &&
        thread_wake_handler_(process_.pid, slot)) {
      return;
    }
    userland_hle_.unbind_thread_callback(*callback->processor);
    if (thread_terminate_handler_) {
      static_cast<void>(
          thread_terminate_handler_(process_.pid, *callback->processor));
    }
    core_audio_hle_.io_proc_schedule_failed(callback->process_id,
                                            callback->native,
                                            callback->io_proc_id);
    output_.write("[coreaudio-device] io-proc wake failed pid=" +
                  std::to_string(process_.pid) + "\n");
    return;
  }
  const auto processor =
      thread_create_handler_(callback->registers, callback->cpsr);
  if (!processor || !update_thread_pointer(*processor) ||
      !userland_hle_.bind_thread_callback(*processor,
                                          std::move(callback->completion))) {
    if (processor && thread_terminate_handler_) {
      static_cast<void>(thread_terminate_handler_(process_.pid, *processor));
    }
    core_audio_hle_.io_proc_schedule_failed(callback->process_id,
                                            callback->native,
                                            callback->io_proc_id);
    output_.write("[coreaudio-device] io-proc schedule failed pid=" +
                  std::to_string(process_.pid) + "\n");
    return;
  }
  core_audio_hle_.io_proc_thread_scheduled(callback->process_id,
                                           callback->native,
                                           callback->io_proc_id, *processor);
}

void CompatibilityKernel::inject_wifi_driver_event(
    std::uint32_t, std::uint32_t event) {
  if (event == 0) return;
  namespace wifi_driver = darwin::network::apple80211_driver;
  for (const auto& [descriptor, kind] : virtual_descriptors_) {
    if (kind == wifi_driver::event_descriptor_kind) {
      auto& stream = wifi_driver_event_streams_[descriptor];
      if (!stream)
        stream = std::make_shared<wifi_driver::EventStream>();
      stream->enqueue(event);
    }
  }
}

void CompatibilityKernel::reap_stopped_audio_threads() {
  for (const auto processor : core_audio_hle_.take_retired_io_proc_threads()) {
    userland_hle_.unbind_thread_callback(processor);
    if (thread_terminate_handler_) {
      static_cast<void>(
          thread_terminate_handler_(process_.pid, processor));
    }
  }
}

void CompatibilityKernel::advance_time_by(std::uint64_t interval) {
  const auto now = shared_state_->clock.now();
  const auto deadline =
      interval > std::numeric_limits<std::uint64_t>::max() - now
          ? std::numeric_limits<std::uint64_t>::max()
          : now + interval;
  advance_absolute_time(deadline);
}

void CompatibilityKernel::attach(Cpu &cpu) {
  cpu.set_svc_handler([this](Cpu &source, std::uint32_t immediate) {
    dispatch(source, immediate);
  });
}

std::optional<std::uint32_t>
CompatibilityKernel::thread_object_for_processor(
    std::size_t processor) const {
  std::lock_guard mach_lock{shared_state_->mach_mutex};
  if (const auto task =
          shared_state_->task_thread_port_objects.find(process_.pid);
      task != shared_state_->task_thread_port_objects.end()) {
    if (const auto thread = task->second.find(
            static_cast<std::uint32_t>(processor));
        thread != task->second.end()) {
      return thread->second;
    }
  }
  return std::nullopt;
}

void CompatibilityKernel::clear_thread_io_policy(std::size_t processor_id) {
  if (const auto thread_object = thread_object_for_processor(processor_id)) {
    process_.thread_disk_io_policies.erase(*thread_object);
  }
}

void CompatibilityKernel::inherit_process_state(
    const CompatibilityKernel &parent, std::uint32_t child_pid,
    ProcessInheritance inheritance) {
  const auto inherit_fork_state = inheritance == ProcessInheritance::Fork;
  shared_state_ = parent.shared_state_;
  display_state_ = parent.display_state_;
  presentation_tracker_ = parent.presentation_tracker_;
  scene_coordinator_ = parent.scene_coordinator_;
  // posix_spawn starts with a fresh virtual address space, not a fresh device
  // namespace. CoreSurface IDs are global transport names and must still
  // resolve to the producer's shared backing when firmware sends them to a
  // compositor task.
  surface_store_->share_registry(*parent.surface_store_);
  wifi_state_ = parent.wifi_state_;
  audio_service_ = parent.audio_service_;
  ringer_switch_state_ = parent.ringer_switch_state_;
  if (inherit_fork_state) {
    darwin_notify_state_hle_.inherit_state(parent.darwin_notify_state_hle_);
  }
  configure_darwin_notify_state();
  core_audio_hle_.set_service(audio_service_);
  apple80211_hle_.set_wifi_state(wifi_state_);
  core_surface_hle_.set_display(display_state_);
  core_surface_hle_.set_presentation_tracker(presentation_tracker_);
  core_surface_hle_.set_shared_state(shared_state_);
  core_surface_hle_.set_scene_coordinator(scene_coordinator_);
  opengles_hle_.set_shared_state(shared_state_);
  opengles_hle_.set_scene_coordinator(scene_coordinator_);
  mbx2d_hle_.set_shared_state(shared_state_);
  mobile_framebuffer_hle_.set_shared_state(shared_state_);
  mobile_framebuffer_hle_.set_presentation_tracker(presentation_tracker_);
  mobile_framebuffer_hle_.set_scene_coordinator(scene_coordinator_);
  layerkit_hle_.set_shared_state(shared_state_);
  layerkit_hle_.set_scene_coordinator(scene_coordinator_);
  opengles_hle_.set_display(display_state_);
  mbx2d_hle_.set_display(display_state_);
  mbx2d_hle_.set_presentation_tracker(presentation_tracker_);
  mobile_framebuffer_hle_.set_display(display_state_);
  if (inherit_fork_state) {
    userland_hle_.inherit_mappings(parent.userland_hle_);
    apple80211_hle_.inherit_state(parent.apple80211_hle_,
                                  parent.process_.pid, child_pid);
    core_surface_hle_.inherit_state(parent.core_surface_hle_);
    opengles_hle_.inherit_state(parent.opengles_hle_);
    mbx2d_hle_.inherit_state(parent.mbx2d_hle_);
    mobile_framebuffer_hle_.inherit_state(parent.mobile_framebuffer_hle_);
    layerkit_hle_.inherit_state(parent.layerkit_hle_);
  }
  guest_working_directory_ = parent.guest_working_directory_;
  if (inherit_fork_state)
    process_image_ = parent.process_image_;
  process_ = parent.process_;
  process_.parent_pid = parent.process_.pid;
  process_.pid = child_pid;
  process_.exited = false;
  process_.waiting_for_events = false;
  process_.exit_status = 0;
  process_.termination_signal = 0;
  process_.host_port = parent.process_.host_port;
  process_.clock_port = parent.process_.clock_port;
  process_.calendar_clock_port = parent.process_.calendar_clock_port;
  process_.io_master_port = parent.process_.io_master_port;
  process_.io_registry_options_port = parent.process_.io_registry_options_port;
  // fork/posix_spawn creates a new current uthread. Process policy is copied
  // with proc, but a stale parent uthread policy must not follow its new
  // kernel thread object.
  process_.thread_disk_io_policies.clear();
  file_descriptors_ = parent.file_descriptors_;
  regular_file_open_descriptions_ = parent.regular_file_open_descriptions_;
  virtual_block_descriptors_ = parent.virtual_block_descriptors_;
  file_offsets_ = parent.file_offsets_;
  file_status_flags_ = parent.file_status_flags_;
  descriptor_flags_ = parent.descriptor_flags_;
  virtual_descriptors_ = parent.virtual_descriptors_;
  wifi_driver_event_streams_ = parent.wifi_driver_event_streams_;
  offline_serial_state_.inherit_configuration(parent.offline_serial_state_);
  bpf_descriptors_ = parent.bpf_descriptors_;
  host_sockets_ = parent.host_sockets_;
  virtual_udp_sockets_ = parent.virtual_udp_sockets_;
  kernel_control_endpoints_ = parent.kernel_control_endpoints_;
  host_network_policy_ = parent.host_network_policy_;
  bound_socket_names_ = parent.bound_socket_names_;
  listening_sockets_ = parent.listening_sockets_;
  unix_listener_states_ = parent.unix_listener_states_;
  socket_options_ = parent.socket_options_;
  duplicated_descriptors_ = parent.duplicated_descriptors_;
  system_event_filters_ = parent.system_event_filters_;
  apple80211_scan_delivered_ = parent.apple80211_scan_delivered_;
  system_event_next_identifiers_ = parent.system_event_next_identifiers_;
  route_socket_states_ = parent.route_socket_states_;
  socket_pair_endpoints_ = parent.socket_pair_endpoints_;
  if (inherit_fork_state) {
    vm_purgable_states_ = parent.vm_purgable_states_;
    signal_actions_ = parent.signal_actions_;
    signal_mask_ = parent.signal_mask_;
  }
  kqueues_ = parent.kqueues_;
  random_state_ = parent.random_state_ ^ child_pid;
  thread_ports_.clear();
  thread_ports_.emplace(0, process_.thread_port);
  {
    std::lock_guard mach_lock{shared_state_->mach_mutex};
    if (!mach_task_identity::inherit_child(*shared_state_, parent.process_,
                                           process_)) {
      throw std::runtime_error{"failed to inherit child Mach task identity"};
    }
  }
  const auto parent_record = shared_state_->processes.find(parent.process_.pid);
  auto child_record = parent_record != shared_state_->processes.end()
                          ? parent_record->second
                          : KernelSharedState::ProcessRecord{};
  child_record.parent_pid = process_.parent_pid;
  child_record.process_group = process_.process_group;
  child_record.uid = process_.uid;
  child_record.effective_uid = process_.effective_uid;
  child_record.gid = process_.gid;
  child_record.effective_gid = process_.effective_gid;
  child_record.exit_status = 0;
  child_record.termination_signal = 0;
  child_record.exited = false;
  if (child_record.command.empty())
    child_record.command = "unknown";
  shared_state_->processes[child_pid] = std::move(child_record);
}

void CompatibilityKernel::dispatch(Cpu &cpu, std::uint32_t svc_immediate) {
  std::lock_guard lock{mutex_};
  if (apple80211_hle_.deliver_pending_event(
          cpu, process_.pid, svc_immediate)) {
    output_.write("[wifi-service] event-deliver pid=" +
                  std::to_string(process_.pid) + " cpu=" +
                  std::to_string(cpu.processor_id()) + "\n");
    return;
  }
  if (userland_hle_.dispatch(cpu, process_.pid, svc_immediate)) {
    reap_stopped_audio_threads();
    return;
  }
  if (svc_immediate != 0x80) {
    trace_unknown(cpu, "SVC", svc_immediate);
    // An unexpected SVC immediate is an invalid instruction encoding for the
    // supported ARM userspace ABI, rather than an unimplemented kernel entry.
    // Keep that fatal policy explicit and separate from unknown-ABI tracing.
    cpu.halt(Dynarmic::HaltReason::UserDefined4);
    return;
  }
  if (cpu.registers()[12] == darwin::arm_fast_trap::syscall_number) {
    dispatch_arm_fast_trap(cpu);
    return;
  }
  const auto number = static_cast<std::int32_t>(cpu.registers()[12]);
  if (number < 0) {
    dispatch_mach(
        cpu, static_cast<std::uint32_t>(-static_cast<std::int64_t>(number)));
  } else {
    dispatch_bsd(cpu, static_cast<std::uint32_t>(number));
  }
}

void CompatibilityKernel::dispatch_arm_fast_trap(Cpu &cpu) {
  auto &registers = cpu.registers();
  switch (registers[3]) {
  case darwin::arm_fast_trap::instruction_cache_invalidate: {
    const auto address = registers[0];
    const auto length = registers[1];
    if (length != 0 &&
        length - 1U <= std::numeric_limits<std::uint32_t>::max() - address) {
      // iPhoneOS 1.0's UIKit emits ARM trampolines into writable heap
      // pages, calls this trap, and immediately branches to them without
      // a vm_protect/mprotect transition. Preserve that first-generation
      // ARM behavior by making an already-mapped invalidated range
      // executable; never create memory as a side effect of the trap.
      if (memory_.mapped(address, length)) {
        static_cast<void>(
            memory_.map(address, length, MemoryPermission::Execute));
      }
      cpu.invalidate_cache_range(address, length);
    }
    // XNU returns from these cache traps through the original saved state,
    // so all user registers (including r0) remain unchanged.
    return;
  }
  case darwin::arm_fast_trap::data_cache_flush:
    // Guest writes already update the coherent AddressSpace immediately.
    // Preserve the saved registers just as the real trap return path does.
    return;
  case darwin::arm_fast_trap::thread_set_cthread_self:
    cpu.set_cthread_self(registers[0]);
    return;
  case darwin::arm_fast_trap::thread_get_cthread_self:
    registers[0] = cpu.cthread_self().value_or(0);
    return;
  default:
    trace_unknown(cpu, "ARM fast trap", registers[3]);
    return;
  }
}

void CompatibilityKernel::bsd_success(Cpu &cpu, std::uint32_t value,
                                      std::uint32_t second_value) {
  cpu.registers()[0] = value;
  cpu.registers()[1] = second_value;
  cpu.set_cpsr(cpu.cpsr() & ~carry_flag);
}

void CompatibilityKernel::bsd_error(Cpu &cpu, std::uint32_t error) {
  cpu.registers()[0] = error;
  cpu.set_cpsr(cpu.cpsr() | carry_flag);
}

bool CompatibilityKernel::write_guest_stat(std::uint32_t address,
                                           const std::filesystem::path &path,
                                           bool follow_symlink,
                                           int host_descriptor) {
  // Darwin 8 32-bit struct stat is 96 bytes. Keep this explicit instead of
  // copying the host struct: field widths and alignment differ by ABI.
  std::array<std::byte, 96> bytes{};
  auto put16 = [&](std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
  };
  auto put32 = [&](std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
      bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    }
  };
  auto put64 = [&](std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
      bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    }
  };

  auto metadata = query_hfs_metadata(path, follow_symlink);
  if (host_descriptor >= 0) {
    struct stat status {};
    if (::fstat(host_descriptor, &status) != 0)
      return false;
    struct stat path_status {};
    const auto path_result = follow_symlink ? ::stat(path.c_str(), &path_status)
                                            : ::lstat(path.c_str(), &path_status);
    if (path_result != 0 || path_status.st_dev != status.st_dev ||
        path_status.st_ino != status.st_ino) {
      hfs::Metadata open_file;
      open_file.name = path.filename().string();
      open_file.catalog_id = static_cast<std::uint32_t>(status.st_ino);
      open_file.permanent_id = open_file.catalog_id;
      open_file.mode = static_cast<std::uint32_t>(status.st_mode);
      open_file.owner = static_cast<std::uint32_t>(status.st_uid);
      open_file.group = static_cast<std::uint32_t>(status.st_gid);
      open_file.link_count = static_cast<std::uint32_t>(status.st_nlink);
      open_file.data_length = static_cast<std::uint64_t>(status.st_size);
      open_file.data_allocation_size =
          static_cast<std::uint64_t>(status.st_blocks) * 512U;
      open_file.directory = S_ISDIR(status.st_mode);
      metadata = std::move(open_file);
    }
  }
  if (!metadata)
    return false;
  put32(0, root_disk_device);                           // st_dev
  put32(4, metadata->permanent_id);                     // st_ino
  put16(8, static_cast<std::uint16_t>(metadata->mode)); // st_mode
  put16(10, static_cast<std::uint16_t>(
                std::min(metadata->link_count,
                         static_cast<std::uint32_t>(
                             std::numeric_limits<std::uint16_t>::max()))));
  put32(12, metadata->owner); // st_uid
  put32(16, metadata->group); // st_gid
  put32(20, 0);               // st_rdev
  put32(24, static_cast<std::uint32_t>(metadata->access_time.seconds));
  put32(28, static_cast<std::uint32_t>(metadata->access_time.nanoseconds));
  put32(32, static_cast<std::uint32_t>(metadata->modification_time.seconds));
  put32(36,
        static_cast<std::uint32_t>(metadata->modification_time.nanoseconds));
  put32(40, static_cast<std::uint32_t>(metadata->change_time.seconds));
  put32(44, static_cast<std::uint32_t>(metadata->change_time.nanoseconds));
  put64(48, metadata->data_length);                          // st_size
  put64(56, (metadata->data_allocation_size + 511U) / 512U); // st_blocks
  put32(64, AddressSpace::page_size);                        // st_blksize
  put32(68, metadata->flags);                                // st_flags
  return memory_.copy_in(address, bytes);
}

bool CompatibilityKernel::write_guest_device_stat(std::uint32_t address,
                                                  std::uint32_t minor,
                                                  bool character_device) {
  std::array<std::byte, 96> bytes{};
  const auto put16 = [&](std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
  };
  const auto put32 = [&](std::size_t offset, std::uint32_t value) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
      bytes[offset + byte] = static_cast<std::byte>(value >> (byte * 8U));
    }
  };
  put32(0, 1);              // st_dev
  put32(4, 0x100U + minor); // stable virtual inode
  put16(8, static_cast<std::uint16_t>((character_device ? 0020000U : 0060000U) |
                                      0640U));
  put16(10, 1);                                   // st_nlink
  put32(20, (virtual_disk_major << 24U) | minor); // Darwin device rdev
  put32(64, AddressSpace::page_size);             // st_blksize
  return memory_.copy_in(address, bytes);
}

bool CompatibilityKernel::write_guest_stat64(std::uint32_t address,
                                             const std::filesystem::path &path,
                                             bool follow_symlink,
                                             int host_descriptor) {
  // Darwin 9 ARM32 struct stat64 keeps 64-bit inode, size, block count, and
  // qspare fields, while time_t/long timespec members remain 32-bit and
  // 64-bit integers are only 4-byte aligned on this firmware ABI.
  std::array<std::byte, 108> bytes{};
  auto put16 = [&](std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
  };
  auto put32 = [&](std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
      bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    }
  };
  auto put64 = [&](std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
      bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    }
  };
  auto put_time = [&](std::size_t offset, const hfs::Timestamp &timestamp) {
    put32(offset, static_cast<std::uint32_t>(timestamp.seconds));
    put32(offset + 4U, static_cast<std::uint32_t>(timestamp.nanoseconds));
  };

  auto metadata = query_hfs_metadata(path, follow_symlink);
  if (host_descriptor >= 0) {
    struct stat status {};
    if (::fstat(host_descriptor, &status) != 0)
      return false;
    struct stat path_status {};
    const auto path_result = follow_symlink ? ::stat(path.c_str(), &path_status)
                                            : ::lstat(path.c_str(), &path_status);
    if (path_result != 0 || path_status.st_dev != status.st_dev ||
        path_status.st_ino != status.st_ino) {
      hfs::Metadata open_file;
      open_file.name = path.filename().string();
      open_file.catalog_id = static_cast<std::uint32_t>(status.st_ino);
      open_file.permanent_id = open_file.catalog_id;
      open_file.mode = static_cast<std::uint32_t>(status.st_mode);
      open_file.owner = static_cast<std::uint32_t>(status.st_uid);
      open_file.group = static_cast<std::uint32_t>(status.st_gid);
      open_file.link_count = static_cast<std::uint32_t>(status.st_nlink);
      open_file.data_length = static_cast<std::uint64_t>(status.st_size);
      open_file.data_allocation_size =
          static_cast<std::uint64_t>(status.st_blocks) * 512U;
      open_file.directory = S_ISDIR(status.st_mode);
      metadata = std::move(open_file);
    }
  }
  if (!metadata)
    return false;

  put32(0, root_disk_device);                           // st_dev
  put16(4, static_cast<std::uint16_t>(metadata->mode)); // st_mode
  put16(6, static_cast<std::uint16_t>(
               std::min(metadata->link_count,
                        static_cast<std::uint32_t>(
                            std::numeric_limits<std::uint16_t>::max()))));
  put64(8, metadata->permanent_id); // st_ino
  put32(16, metadata->owner);
  put32(20, metadata->group);
  put32(24, 0); // st_rdev
  put_time(28, metadata->access_time);
  put_time(36, metadata->modification_time);
  put_time(44, metadata->change_time);
  put_time(52, metadata->creation_time);
  put64(60, metadata->data_length);
  put64(68, (metadata->data_allocation_size + 511U) / 512U);
  put32(76, AddressSpace::page_size);
  put32(80, metadata->flags);
  return memory_.copy_in(address, bytes);
}

bool CompatibilityKernel::write_guest_device_stat64(std::uint32_t address,
                                                    std::uint32_t minor,
                                                    bool character_device) {
  std::array<std::byte, 108> bytes{};
  const auto put16 = [&](std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
  };
  const auto put32 = [&](std::size_t offset, std::uint32_t value) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
      bytes[offset + byte] = static_cast<std::byte>(value >> (byte * 8U));
    }
  };
  const auto put64 = [&](std::size_t offset, std::uint64_t value) {
    for (std::size_t byte = 0; byte < 8; ++byte) {
      bytes[offset + byte] = static_cast<std::byte>(value >> (byte * 8U));
    }
  };
  put32(0, 1);              // st_dev
  put16(4, static_cast<std::uint16_t>((character_device ? 0020000U : 0060000U) |
                                      0640U));
  put16(6, 1);              // st_nlink
  put64(8, 0x100U + minor); // stable virtual inode
  put32(24, (virtual_disk_major << 24U) | minor);
  put32(76, AddressSpace::page_size);
  return memory_.copy_in(address, bytes);
}

bool CompatibilityKernel::write_guest_statfs(
    std::uint32_t address, const hfs::VolumeMetadata &volume) {
  // Darwin 8's 32-bit legacy statfs layout is 272 bytes.
  std::array<std::byte, 272> bytes{};
  const auto put16 = [&](std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
  };
  const auto put32 = [&](std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
      bytes[offset + index] =
          static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
  };
  const auto put_string = [&](std::size_t offset, std::size_t capacity,
                              std::string_view value) {
    const auto count = std::min(capacity - 1, value.size());
    for (std::size_t index = 0; index < count; ++index) {
      bytes[offset + index] = static_cast<std::byte>(value[index]);
    }
  };
  put16(2, static_cast<std::uint16_t>(volume.mount_flags));
  put32(4, volume.block_size);
  put32(8, volume.io_block_size);
  put32(12, volume.total_blocks);
  put32(16, volume.free_blocks);
  put32(20, volume.free_blocks);
  put32(24, 0xffff'ffffU); // HFS reports no practical inode ceiling
  put32(28, 0xffff'ffffU - volume.next_catalog_id);
  put32(32, 1); // fsid[0]
  put32(48, volume.mount_flags);
  put_string(60, 15, "hfs");
  put_string(75, 90, volume.mount_point);
  put_string(165, 90, volume.mounted_device);
  return memory_.copy_in(address, bytes);
}

bool CompatibilityKernel::write_guest_statfs64(
    std::uint32_t address, const hfs::VolumeMetadata &volume) {
  // Darwin 9's 32-bit statfs64 ABI.  See xnu-1228 bsd/sys/mount.h:
  // two 32-bit sizes, five 64-bit counters, fsid_t, owner/type/flags/subtype,
  // then fixed-size type and path strings.
  std::array<std::byte, 2168> bytes{};
  const auto put32 = [&](std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
      bytes[offset + index] =
          static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
  };
  const auto put64 = [&](std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
      bytes[offset + index] =
          static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
  };
  const auto put_string = [&](std::size_t offset, std::size_t capacity,
                              std::string_view value) {
    const auto count = std::min(capacity - 1, value.size());
    for (std::size_t index = 0; index < count; ++index) {
      bytes[offset + index] = static_cast<std::byte>(value[index]);
    }
  };

  put32(0, volume.block_size);
  put32(4, volume.io_block_size);
  put64(8, volume.total_blocks);
  put64(16, volume.free_blocks);
  put64(24, volume.free_blocks);
  put64(32, 0xffff'ffffULL); // HFS reports no practical inode ceiling
  put64(40, 0xffff'ffffULL - volume.next_catalog_id);
  put32(48, 1); // fsid[0]
  put32(56, 0); // owner
  put32(60, volume.filesystem_type);
  put32(64, volume.mount_flags);
  put32(68, 0); // f_fssubtype
  put_string(72, 16, "hfs");
  put_string(88, 1024, volume.mount_point);
  put_string(1112, 1024, volume.mounted_device);
  return memory_.copy_in(address, bytes);
}

std::filesystem::path
CompatibilityKernel::resolve_guest_path(const std::string &path,
                                        bool follow_final_symlink) const {
  constexpr std::string_view resource_fork_suffix{"/..namedfork/rsrc"};
  if (path.ends_with(resource_fork_suffix)) {
    auto data_path = path.substr(0, path.size() - resource_fork_suffix.size());
    if (data_path.empty())
      data_path = "/";
    return hfs::MetadataProvider::resource_sidecar(
        resolve_guest_path(data_path, true));
  }
  return RootfsPathResolver{rootfs_}.resolve(
      path, guest_working_directory_, follow_final_symlink);
}

std::optional<hfs::Metadata>
CompatibilityKernel::query_hfs_metadata(const std::filesystem::path &path,
                                        bool follow_symlink) const {
  auto metadata = hfs_metadata_.query(path, follow_symlink);
  if (!metadata)
    return std::nullopt;
  const std::lock_guard filesystem_lock{shared_state_->filesystem_mutex};
  if (const auto override =
          shared_state_->hfs_metadata_overrides.find(metadata->permanent_id);
      override != shared_state_->hfs_metadata_overrides.end()) {
    hfs::MetadataProvider::apply_override(*metadata, override->second);
  }
  return metadata;
}

std::optional<std::vector<std::byte>>
CompatibilityKernel::query_hfs_named_attribute(
    const std::filesystem::path &path, bool follow_symlink,
    std::string_view name) const {
  const auto metadata = hfs_metadata_.query(path, follow_symlink);
  if (!metadata)
    return std::nullopt;
  {
    const std::lock_guard filesystem_lock{shared_state_->filesystem_mutex};
    if (const auto inode = shared_state_->hfs_named_attribute_overrides.find(
            metadata->permanent_id);
        inode != shared_state_->hfs_named_attribute_overrides.end()) {
      if (const auto attribute = inode->second.find(std::string{name});
          attribute != inode->second.end()) {
        return attribute->second;
      }
    }
  }
  return hfs_metadata_.named_attribute(path, name, follow_symlink);
}

std::vector<std::string> CompatibilityKernel::query_hfs_named_attributes(
    const std::filesystem::path &path, bool follow_symlink) const {
  const auto metadata = hfs_metadata_.query(path, follow_symlink);
  if (!metadata)
    return {};
  std::set<std::string> names;
  for (auto &name : hfs_metadata_.named_attributes(path, follow_symlink)) {
    names.emplace(std::move(name));
  }
  {
    const std::lock_guard filesystem_lock{shared_state_->filesystem_mutex};
    if (const auto inode = shared_state_->hfs_named_attribute_overrides.find(
            metadata->permanent_id);
        inode != shared_state_->hfs_named_attribute_overrides.end()) {
      for (const auto &[name, value] : inode->second) {
        if (value)
          names.emplace(name);
        else
          names.erase(name);
      }
    }
  }
  return {names.begin(), names.end()};
}

std::uint32_t CompatibilityKernel::file_descriptor_limit() const {
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(
      process_.resource_limits[darwin::resource::open_files].current,
      darwin::resource::maximum_open_files));
}

std::optional<std::uint32_t>
CompatibilityKernel::allocate_file_descriptor() const {
  for (std::uint32_t descriptor = 3; descriptor < file_descriptor_limit();
       ++descriptor) {
    if (!file_descriptors_.contains(descriptor) &&
        !virtual_descriptors_.contains(descriptor) &&
        !duplicated_descriptors_.contains(descriptor)) {
      return descriptor;
    }
  }
  return std::nullopt;
}

void CompatibilityKernel::trace_unknown(Cpu &cpu, std::string kind,
                                        std::uint32_t number) {
  std::ostringstream message;
  message << "[kernel] unsupported " << kind << ' ' << number << " pc=0x"
          << std::hex << cpu.registers()[15];
  for (std::size_t index = 0; index < 7; ++index) {
    message << " r" << std::dec << index << "=0x" << std::hex
            << cpu.registers()[index];
  }
  message << std::dec << '\n';
  output_.write(message.str());
}

} // namespace ilemu
