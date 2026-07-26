#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ilemu/address_space.hpp"
#include "ilemu/audio.hpp"
#include "ilemu/audio_toolbox_hle.hpp"
#include "ilemu/core_audio_hle.hpp"
#include "ilemu/apple80211_hle.hpp"
#include "ilemu/core_surface_hle.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_bpf_abi.hpp"
#include "ilemu/darwin_notify_state_hle.hpp"
#include "ilemu/darwin_tty_abi.hpp"
#include "ilemu/display.hpp"
#include "ilemu/device_profile.hpp"
#include "ilemu/hfs_metadata.hpp"
#include "ilemu/host_network.hpp"
#include "ilemu/kernel_control.hpp"
#include "ilemu/kernel_shared_state.hpp"
#include "ilemu/layerkit_hle.hpp"
#include "ilemu/mach_arm_thread_abi.hpp"
#include "ilemu/mbx2d_hle.hpp"
#include "ilemu/mobile_framebuffer_hle.hpp"
#include "ilemu/opengles_hle.hpp"
#include "ilemu/offline_serial_device.hpp"
#include "ilemu/output.hpp"
#include "ilemu/presentation_tracker.hpp"
#include "ilemu/ringer_switch_state.hpp"
#include "ilemu/scene_coordinator.hpp"
#include "ilemu/surface_store.hpp"
#include "ilemu/system_button_input.hpp"
#include "ilemu/touch_input.hpp"
#include "ilemu/userland_hle.hpp"
#include "ilemu/virtual_udp.hpp"
#include "ilemu/wifi_state.hpp"

namespace ilemu {

class CompatibilityKernel {
public:
  struct SchedulerYieldRequest {
    bool depress{};
    std::uint32_t duration_milliseconds{};
  };
  struct WaitChildResult {
    bool has_child{};
    std::optional<std::uint32_t> child_pid;
    std::uint32_t status{};
  };
  using ThreadCreateHandler = std::function<std::optional<std::size_t>(
      const std::array<std::uint32_t, 16> &, std::uint32_t)>;
  using ThreadTerminateHandler =
      std::function<bool(std::uint32_t, std::size_t)>;
  using ThreadStateQuery =
      std::function<std::optional<darwin::arm_thread::GeneralState>(
          std::uint32_t, std::uint32_t, std::uint32_t)>;
  using ThreadStateUpdateHandler = std::function<bool(
      std::uint32_t, std::uint32_t,
      const darwin::arm_thread::GeneralState &)>;
  using ThreadRunnableHandler =
      std::function<bool(std::uint32_t, std::uint32_t, bool)>;
  using ThreadWakeHandler =
      std::function<bool(std::uint32_t, std::uint32_t)>;
  using ForkHandler = std::function<std::optional<std::uint32_t>(Cpu &)>;
  using ExecHandler = std::function<bool(
      Cpu &, std::string, std::vector<std::string>, std::vector<std::string>)>;
  using SpawnExecHandler =
      std::function<bool(std::uint32_t, std::string, std::vector<std::string>,
                         std::vector<std::string>, bool)>;
  using SchedulerRunnableQuery = std::function<bool(std::size_t)>;
  using LegacyThreadPolicyHandler = std::function<bool(
      std::size_t, std::uint32_t, std::int32_t, bool)>;
  using ThreadPolicyHandler = std::function<bool(
      std::size_t, std::uint32_t, std::span<const std::uint32_t>)>;
  using TaskPriorityHandler = std::function<void(std::int32_t)>;
  using SchedulerPreemptionQuery = std::function<bool(std::size_t)>;
  using SignalDeliveryHandler =
      std::function<std::uint32_t(std::uint32_t, std::uint32_t)>;
  using WaitChildHandler =
      std::function<WaitChildResult(std::int32_t, bool)>;

  CompatibilityKernel(AddressSpace &memory, Output &output,
                      std::filesystem::path rootfs = {},
                      DeviceProfile device = DeviceProfile::default_profile(),
                      std::optional<bool> activated = std::nullopt);

  void attach(Cpu &cpu);
  void dispatch(Cpu &cpu, std::uint32_t svc_immediate);

  [[nodiscard]] ProcessContext &process() { return process_; }
  [[nodiscard]] const ProcessContext &process() const { return process_; }
  void exit_process(std::uint32_t status, std::uint32_t signal = 0);
  bool reap_process();
  void set_halt_on_unknown(bool value) { halt_on_unknown_ = value; }
  void set_thread_create_handler(ThreadCreateHandler handler) {
    thread_create_handler_ = std::move(handler);
  }
  void set_thread_terminate_handler(ThreadTerminateHandler handler) {
    thread_terminate_handler_ = std::move(handler);
  }
  void set_thread_state_query(ThreadStateQuery query) {
    thread_state_query_ = std::move(query);
  }
  void set_thread_state_update_handler(ThreadStateUpdateHandler handler) {
    thread_state_update_handler_ = std::move(handler);
  }
  void set_thread_runnable_handler(ThreadRunnableHandler handler) {
    thread_runnable_handler_ = std::move(handler);
  }
  void set_thread_wake_handler(ThreadWakeHandler handler) {
    thread_wake_handler_ = std::move(handler);
  }
  void set_fork_handler(ForkHandler handler) {
    fork_handler_ = std::move(handler);
  }
  void set_exec_handler(ExecHandler handler) {
    exec_handler_ = std::move(handler);
  }
  void set_spawn_exec_handler(SpawnExecHandler handler) {
    spawn_exec_handler_ = std::move(handler);
  }
  void set_scheduler_runnable_query(SchedulerRunnableQuery query) {
    scheduler_runnable_query_ = std::move(query);
  }
  void set_legacy_thread_policy_handler(LegacyThreadPolicyHandler handler) {
    legacy_thread_policy_handler_ = std::move(handler);
  }
  void set_thread_policy_handler(ThreadPolicyHandler handler) {
    thread_policy_handler_ = std::move(handler);
  }
  void set_task_priority_handler(TaskPriorityHandler handler) {
    task_priority_handler_ = std::move(handler);
  }
  void set_scheduler_preemption_query(SchedulerPreemptionQuery query) {
    scheduler_preemption_query_ = std::move(query);
  }
  void set_signal_delivery_handler(SignalDeliveryHandler handler) {
    signal_delivery_handler_ = std::move(handler);
  }
  void set_wait_child_handler(WaitChildHandler handler) {
    wait_child_handler_ = std::move(handler);
  }
  [[nodiscard]] std::uint32_t deliver_signal(std::uint32_t signal);
  [[nodiscard]] std::optional<SchedulerYieldRequest>
  consume_scheduler_yield(std::size_t processor_id);
  void set_display_presenter(DisplayState::Presenter presenter) {
    display_state_->set_presenter(std::move(presenter));
  }
  void set_audio_sink(std::shared_ptr<AudioSink> sink) {
    audio_service_->set_sink(std::move(sink));
  }
  void set_audio_decoder(std::shared_ptr<AudioDecoder> decoder) {
    audio_service_->set_decoder(std::move(decoder));
  }
  [[nodiscard]] DisplayFrame display_snapshot() const {
    return display_state_->snapshot();
  }
  [[nodiscard]] std::optional<std::uint32_t>
  active_client_process_id() const {
    const auto active_scene = scene_coordinator_->active_client_scene();
    return active_scene
               ? std::optional<std::uint32_t>{active_scene->client_process_id}
               : std::nullopt;
  }
  [[nodiscard]] std::uint64_t current_absolute_time() const {
    return shared_state_->clock.now();
  }
  [[nodiscard]] std::uint64_t current_wall_time() const {
    return shared_state_->clock.wall_time();
  }
  [[nodiscard]] std::size_t bootstrap_checked_in_service_count() const;
  void set_wall_time(std::uint64_t unix_time_nanoseconds) {
    shared_state_->clock.set_wall_time(unix_time_nanoseconds);
  }
  void synchronize_wall_time(std::uint64_t unix_time_nanoseconds) {
    set_wall_time(unix_time_nanoseconds);
  }
  // Refreshes the process-local firmware framebuffer into the shared host
  // display. Most processes have no scanout surface and return immediately.
  bool refresh_display_scanout();
  bool set_virtual_processor_count(std::size_t processor_count);
  void set_preferred_wifi_networks(std::vector<std::string> ssids) {
    wifi_state_->set_preferred_networks(std::move(ssids));
  }
  void set_host_network_policy(HostNetworkPolicy policy);
  [[nodiscard]] WifiSnapshot wifi_snapshot() const {
    return wifi_state_->snapshot();
  }
  [[nodiscard]] std::optional<darwin::network::InterfaceSnapshot>
  network_interface_snapshot(std::string_view name) const;
  [[nodiscard]] std::vector<darwin::route::Entry> route_snapshot() const;
  void enqueue_baseband_input(std::span<const std::byte> bytes);
  void enqueue_touch_input(const TouchInput &input);
  void enqueue_system_button(const SystemButtonInput &input);
  void set_ringer_switch_active(bool active);
  void toggle_ringer_switch();
  [[nodiscard]] bool display_powered_on() const;
  [[nodiscard]] std::vector<std::byte> take_baseband_output();
  void inherit_process_state(const CompatibilityKernel &parent,
                             std::uint32_t child_pid);
  void prepare_exec(std::size_t processor_id);
  void install_main_image_hle(Cpu &cpu,
                              std::string_view mapped_guest_path = {});
  void set_process_image(std::string_view guest_path);
  void set_process_arguments(const std::vector<std::string> &arguments,
                             const std::vector<std::string> &environment);
  [[nodiscard]] const std::map<std::size_t, PendingWait> &
  pending_waits() const {
    return pending_waits_;
  }
  bool complete_wait(Cpu &cpu, std::uint32_t child_pid,
                     std::uint32_t wait_status);
  bool fail_wait(Cpu &cpu, std::uint32_t error);
  bool deliver_pending_mach(Cpu &cpu);
  bool deliver_pending_io(Cpu &cpu);
  [[nodiscard]] std::optional<std::uint64_t> next_timer_deadline() const;
  void advance_absolute_time(std::uint64_t deadline);
  void advance_time_by(std::uint64_t interval);
  // The clock is shared by every process, while device registrations are
  // process-local. The boot scheduler calls this for sibling kernels after
  // advancing the shared clock through one representative kernel.
  void service_time_dependent_devices(std::uint64_t deadline);
  [[nodiscard]] std::string wait_reason(std::size_t processor) const;

private:
  struct MachMessageRequest {
    std::uint32_t address{};
    std::uint32_t bits{};
    std::uint32_t remote_port{};
    std::uint32_t local_port{};
    std::uint32_t identifier{};
  };

  struct AioCompletion {
    std::uint32_t descriptor{};
    std::int32_t result{};
    std::uint32_t error{};
  };

  void dispatch_arm_fast_trap(Cpu &cpu);
  void dispatch_bsd(Cpu &cpu, std::uint32_t number);
  void dispatch_bsd_aio(Cpu &cpu, std::uint32_t number);
  void dispatch_bsd_process(Cpu &cpu, std::uint32_t number);
  void release_process_mach_rights();
  [[nodiscard]] bool dispatch_bsd_process_credentials(Cpu &cpu,
                                                      std::uint32_t number);
  [[nodiscard]] bool dispatch_bsd_process_spawn(Cpu &cpu, std::uint32_t number);
  void dispatch_bsd_filesystem(Cpu &cpu, std::uint32_t number);
  [[nodiscard]] bool dispatch_bsd_filesystem_ownership(Cpu &cpu,
                                                       std::uint32_t number);
  [[nodiscard]] bool dispatch_bsd_filesystem_locking(Cpu &cpu,
                                                     std::uint32_t number);
  [[nodiscard]] bool dispatch_bsd_record_locking(Cpu &cpu,
                                                 std::uint32_t command);
  void release_record_locks_for_descriptor(std::uint32_t fd);
  [[nodiscard]] bool dispatch_bsd_filesystem_persistence(Cpu &cpu,
                                                         std::uint32_t number);
  [[nodiscard]] bool dispatch_bsd_filesystem_timestamps(Cpu &cpu,
                                                        std::uint32_t number);
  void dispatch_bsd_descriptor_memory(Cpu &cpu, std::uint32_t number);
  [[nodiscard]] bool dispatch_bsd_shared_region(Cpu &cpu, std::uint32_t number);
  [[nodiscard]] bool dispatch_bsd_debug(Cpu &cpu, std::uint32_t number);
  void dispatch_bsd_socket(Cpu &cpu, std::uint32_t number);
  void dispatch_bsd_events(Cpu &cpu, std::uint32_t number);
  [[nodiscard]] bool ioctl_bpf_device(Cpu &cpu, std::uint32_t fd);
  [[nodiscard]] bool create_kernel_control_socket(Cpu &cpu);
  [[nodiscard]] bool connect_kernel_control_socket(Cpu &cpu);
  [[nodiscard]] bool ioctl_kernel_control_socket(Cpu &cpu);
  [[nodiscard]] bool name_kernel_control_socket(Cpu &cpu, bool peer);
  [[nodiscard]] bool
  write_kernel_control_socket(Cpu &cpu, std::uint32_t fd,
                              std::span<const std::byte> bytes);
  void dispatch_bsd_signal(Cpu &cpu, std::uint32_t number);
  void dispatch_mach(Cpu &cpu, std::uint32_t trap);
  void dispatch_mach_message(Cpu &cpu);
  [[nodiscard]] bool
  dispatch_mach_host_message(Cpu &cpu, const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_processor_message(Cpu &cpu, const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_port_message(Cpu &cpu, const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_port_limit_message(Cpu &cpu, const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_port_membership_message(Cpu &cpu,
                                        const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_port_query_message(Cpu &cpu, const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_task_vm_message(Cpu &cpu, const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_task_enumeration_message(Cpu &cpu,
                                         const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_thread_state_message(Cpu &cpu,
                                     const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_thread_lifecycle_message(Cpu &cpu,
                                         const MachMessageRequest &request);
  void dispatch_mach_thread_self_trap(Cpu &cpu);
  [[nodiscard]] bool
  dispatch_mach_vm_allocate_message(Cpu &cpu,
                                    const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_vm_copy_message(Cpu &cpu, const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_vm_read_message(Cpu &cpu, const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_vm_purgable_message(Cpu &cpu,
                                    const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_vm_memory_entry_message(Cpu &cpu,
                                        const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_vm_map_message(Cpu &cpu, const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_rights_message(Cpu &cpu, const MachMessageRequest &request);
  [[nodiscard]] bool
  dispatch_mach_notification_message(Cpu &cpu,
                                     const MachMessageRequest &request);
  void bsd_success(Cpu &cpu, std::uint32_t value,
                   std::uint32_t second_value = 0);
  void bsd_error(Cpu &cpu, std::uint32_t error);
  void trace_unknown(Cpu &cpu, std::string kind, std::uint32_t number);
  [[nodiscard]] std::filesystem::path
  resolve_guest_path(const std::string &path,
                     bool follow_final_symlink = true) const;
  [[nodiscard]] std::optional<hfs::Metadata>
  query_hfs_metadata(const std::filesystem::path &path,
                     bool follow_symlink) const;
  [[nodiscard]] std::optional<std::vector<std::byte>>
  query_hfs_named_attribute(const std::filesystem::path &path,
                            bool follow_symlink, std::string_view name) const;
  [[nodiscard]] std::vector<std::string>
  query_hfs_named_attributes(const std::filesystem::path &path,
                             bool follow_symlink) const;
  [[nodiscard]] std::uint32_t file_descriptor_limit() const;
  [[nodiscard]] std::optional<std::uint32_t> allocate_file_descriptor() const;
  [[nodiscard]] std::shared_ptr<bsd::RegularFileOpenDescription>
  ensure_regular_file_open_description(std::uint32_t fd);
  bool write_guest_stat(std::uint32_t address,
                        const std::filesystem::path &path,
                        bool follow_symlink = true,
                        int host_descriptor = -1);
  bool write_guest_device_stat(std::uint32_t address, std::uint32_t minor,
                               bool character_device);
  bool write_guest_statfs(std::uint32_t address);
  void install_commpage();
  void configure_darwin_notify_state();
  bool deliver_pending_mach_locked(Cpu &cpu);
  bool receive_socket_message(Cpu &cpu, std::uint32_t fd,
                              std::uint32_t message_address);
  bool send_socket_message(Cpu &cpu, std::uint32_t fd,
                           std::uint32_t message_address);
  bool receive_bpf_bytes(Cpu &cpu, std::uint32_t fd, std::uint32_t address,
                         std::uint32_t size);
  bool write_bpf_bytes(Cpu &cpu, std::uint32_t fd, std::uint32_t address,
                       std::uint32_t size);
  [[nodiscard]] bool bpf_descriptor_readable(std::uint32_t fd) const;
  bool receive_socket_bytes(Cpu &cpu, std::uint32_t fd, std::uint32_t address,
                            std::uint32_t size,
                            std::uint32_t source_address = 0,
                            std::uint32_t source_length_address = 0);
  bool copy_socket_address(std::uint32_t address, std::uint32_t length_address,
                           std::span<const std::byte> socket_address);
  // Completes a local-stream accept when a connection is queued. A false
  // return means the blocking call must remain suspended.
  bool complete_unix_accept(Cpu &cpu, std::uint32_t listener_fd,
                            std::uint32_t address,
                            std::uint32_t length_address);
  [[nodiscard]] std::optional<std::uint32_t>
  install_host_socket(std::shared_ptr<HostSocket> socket);
  void apply_wifi_transition(const WifiSnapshot &before,
                             const WifiSnapshot &after);
  void post_network_event(std::string_view interface_name,
                          std::uint32_t event_subclass,
                          std::uint32_t event_code,
                          std::optional<darwin::network::InterfaceSnapshot>
                              address_snapshot = std::nullopt);
  void post_data_link_event(std::string_view interface_name,
                            std::uint32_t event_code);
  [[nodiscard]] bool
  system_event_matches(std::uint32_t fd,
                       const KernelSharedState::KernelEvent &event) const;
  [[nodiscard]] bool system_event_available(std::uint32_t fd) const;
  [[nodiscard]] std::optional<KernelSharedState::KernelEvent>
  consume_system_event(std::uint32_t fd);
  [[nodiscard]] bool route_message_available(std::uint32_t fd) const;
  [[nodiscard]] std::optional<KernelSharedState::RouteSocketMessage>
  consume_route_message(std::uint32_t fd);
  void post_route_message(
      std::vector<std::byte> bytes, std::uint8_t family,
      std::optional<std::uint64_t> receiver_socket = std::nullopt);
  void synchronize_interface_routes(std::string_view interface_name,
                                    std::uint8_t family);
  [[nodiscard]] std::optional<KernelSharedState::DescriptorTransfer>
  export_descriptor(std::uint32_t fd) const;
  [[nodiscard]] std::optional<std::uint32_t>
  import_descriptor(const KernelSharedState::DescriptorTransfer &transfer);
  [[nodiscard]] bool descriptor_readable(std::uint32_t fd) const;
  [[nodiscard]] bool descriptor_writable(std::uint32_t fd) const;
  [[nodiscard]] std::optional<std::uint32_t>
  socket_pending_byte_count(std::uint32_t fd,
                            std::uint32_t &darwin_error) const;
  [[nodiscard]] std::optional<std::uint32_t>
  collect_ready_kevents(std::uint32_t queue_fd, std::uint32_t event_address,
                        std::uint32_t event_count);
  void detach_kevents_for_descriptor(std::uint32_t fd);
  [[nodiscard]] std::uint32_t
  signal_semaphore_locked(std::uint32_t name, bool all, bool prepost = true);
  [[nodiscard]] std::uint32_t
  signal_semaphore_thread_locked(std::uint32_t semaphore_name,
                                 std::uint32_t thread_name);
  void wait_on_semaphore(Cpu &cpu, std::uint32_t wait_name,
                         std::uint32_t signal_name,
                         std::optional<std::uint64_t> timeout_interval,
                         bool bsd_result);
  void schedule_due_audio_io(std::uint64_t deadline);
  void inject_wifi_driver_event(std::uint32_t descriptor,
                                std::uint32_t event);
  void reap_stopped_audio_threads();

  AddressSpace &memory_;
  Output &output_;
  std::filesystem::path rootfs_;
  DeviceProfile device_profile_;
  hfs::MetadataProvider hfs_metadata_;
  std::shared_ptr<DisplayState> display_state_;
  std::shared_ptr<WifiState> wifi_state_{std::make_shared<WifiState>()};
  std::shared_ptr<AudioService> audio_service_;
  std::shared_ptr<RingerSwitchState> ringer_switch_state_{
      std::make_shared<RingerSwitchState>()};
  UserlandHleRegistry userland_hle_;
  DarwinNotifyStateHle darwin_notify_state_hle_;
  AudioToolboxHle audio_toolbox_hle_;
  CoreAudioHle core_audio_hle_;
  Apple80211Hle apple80211_hle_;
  std::shared_ptr<SurfaceStore> surface_store_{
      std::make_shared<SurfaceStore>()};
  std::shared_ptr<PresentationTracker> presentation_tracker_{
      std::make_shared<PresentationTracker>()};
  std::shared_ptr<SceneCoordinator> scene_coordinator_{
      std::make_shared<SceneCoordinator>()};
  CoreSurfaceHle core_surface_hle_;
  OpenGlesHle opengles_hle_;
  Mbx2dHle mbx2d_hle_;
  MobileFramebufferHle mobile_framebuffer_hle_;
  std::filesystem::path guest_working_directory_{"/"};
  std::string process_image_{"/sbin/launchd"};
  ProcessContext process_;
  std::map<std::uint32_t, std::filesystem::path> file_descriptors_;
  std::map<std::uint32_t, std::shared_ptr<bsd::RegularFileOpenDescription>>
      regular_file_open_descriptions_;
  std::map<std::uint32_t, std::pair<std::uint32_t, bool>>
      virtual_block_descriptors_;
  std::map<std::uint32_t, std::uint64_t> file_offsets_;
  std::map<std::uint32_t, std::uint32_t> file_status_flags_;
  std::map<std::uint32_t, std::uint32_t> descriptor_flags_;
  std::map<std::uint32_t, AioCompletion> aio_completions_;
  std::map<std::uint32_t, std::string> virtual_descriptors_;
  bsd::offline_serial_device::State offline_serial_state_;
  std::map<std::uint32_t, std::shared_ptr<darwin::bpf::DescriptorState>>
      bpf_descriptors_;
  std::map<std::uint32_t, std::shared_ptr<HostSocket>> host_sockets_;
  std::map<std::uint32_t, std::uint16_t> pending_wifi_driver_events_;
  std::map<std::uint32_t, std::shared_ptr<bsd::VirtualUdpSocket>>
      virtual_udp_sockets_;
  std::map<std::uint32_t, std::shared_ptr<bsd::kernel_control::Endpoint>>
      kernel_control_endpoints_;
  std::map<std::uint32_t, std::string> bound_socket_names_;
  std::set<std::uint32_t> listening_sockets_;
  std::map<std::uint32_t, std::shared_ptr<KernelSharedState::UnixListener>>
      unix_listener_states_;
  std::map<std::uint32_t, std::map<std::pair<std::uint32_t, std::uint32_t>,
                                   std::vector<std::byte>>>
      socket_options_;
  std::map<std::uint32_t, std::uint32_t> duplicated_descriptors_;
  std::map<std::uint32_t, std::array<std::uint32_t, 3>> system_event_filters_;
  std::set<std::uint32_t> apple80211_scan_delivered_;
  std::map<std::uint32_t, std::uint32_t> system_event_next_identifiers_;
  std::map<std::uint32_t, std::shared_ptr<KernelSharedState::RouteSocketState>>
      route_socket_states_;
  std::map<std::uint32_t, SocketPairEndpoint> socket_pair_endpoints_;
  std::map<std::uint32_t, std::vector<KeventRegistration>> kqueues_;
  std::map<std::size_t, std::uint32_t> thread_ports_;
  std::map<std::uint32_t, std::uint32_t> vm_purgable_states_;
  std::set<std::size_t> disabled_thread_signals_;
  std::array<std::array<std::uint32_t, 4>, 32> signal_actions_{};
  std::uint32_t signal_mask_{};
  std::uint64_t random_state_{0x69a5'1e8d'4c3b'2701ULL};
  std::shared_ptr<KernelSharedState> shared_state_{
      std::make_shared<KernelSharedState>()};
  ThreadCreateHandler thread_create_handler_;
  ThreadTerminateHandler thread_terminate_handler_;
  ThreadStateQuery thread_state_query_;
  ThreadStateUpdateHandler thread_state_update_handler_;
  ThreadRunnableHandler thread_runnable_handler_;
  ThreadWakeHandler thread_wake_handler_;
  ForkHandler fork_handler_;
  ExecHandler exec_handler_;
  SpawnExecHandler spawn_exec_handler_;
  SchedulerRunnableQuery scheduler_runnable_query_;
  LegacyThreadPolicyHandler legacy_thread_policy_handler_;
  ThreadPolicyHandler thread_policy_handler_;
  TaskPriorityHandler task_priority_handler_;
  SchedulerPreemptionQuery scheduler_preemption_query_;
  SignalDeliveryHandler signal_delivery_handler_;
  WaitChildHandler wait_child_handler_;
  std::map<std::size_t, SchedulerYieldRequest> scheduler_yields_;
  std::map<std::size_t, PendingWait> pending_waits_;
  std::map<std::size_t, PendingMachReceive> pending_mach_receives_;
  std::map<std::size_t, PendingKevent> pending_kevents_;
  std::map<std::size_t, PendingRecvmsg> pending_recvmsgs_;
  std::map<std::size_t, PendingSocketRead> pending_socket_reads_;
  std::map<std::size_t, PendingHostConnect> pending_host_connects_;
  std::map<std::size_t, PendingHostAccept> pending_host_accepts_;
  std::map<std::size_t, PendingUnixAccept> pending_unix_accepts_;
  std::map<std::size_t, PendingFlock> pending_flocks_;
  std::map<std::size_t, PendingRecordLock> pending_record_locks_;
  std::map<std::size_t, PendingSelect> pending_selects_;
  std::map<std::size_t, PendingTimer> pending_timers_;
  std::map<std::size_t, PendingSemaphoreWait> pending_semaphore_waits_;
  std::uint32_t timer_trace_count_{};
  std::uint32_t port_status_trace_count_{};
  std::uint32_t thread_trace_count_{};
  std::uint32_t mapping_trace_count_{};
  std::uint32_t socket_payload_trace_count_{};
  std::uint32_t semaphore_wait_trace_count_{};
  LayerKitHle layerkit_hle_;
  std::uint32_t baseband_io_trace_count_{};
  std::optional<std::uint64_t> next_display_scanout_deadline_;
  bool halt_on_unknown_{true};
  std::uint32_t virtual_processor_count_{1};
  HostNetworkPolicy host_network_policy_{HostNetworkPolicy::Isolated};
  std::mutex mutex_;
};

} // namespace ilemu
