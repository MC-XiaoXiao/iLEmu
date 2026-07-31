#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ilemu/baseband_device.hpp"
#include "ilemu/bsd_file_lock.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
#include "ilemu/display_geometry.hpp"
#include "ilemu/file_page_cache.hpp"
#include "ilemu/hfs_metadata.hpp"
#include "ilemu/kernel_mach_task_identity.hpp"
#include "ilemu/mach_namespace.hpp"
#include "ilemu/mach_port_object.hpp"
#include "ilemu/touch_input.hpp"
#include "ilemu/virtual_clock.hpp"
#include "ilemu/virtual_network.hpp"
#include "ilemu/virtual_udp.hpp"
#include "ilemu/xnu_scheduler.hpp"

namespace ilemu {

class HostSocket;

struct ProcessContext {
  std::uint32_t pid{1};
  std::uint32_t parent_pid{};
  std::uint32_t process_group{};
  std::uint32_t session_id{};
  std::uint32_t uid{};
  std::uint32_t effective_uid{};
  std::uint32_t gid{};
  std::uint32_t effective_gid{};
  std::uint32_t file_creation_mask{0022};
  std::array<darwin::resource::Limit, darwin::resource::limit_count>
      resource_limits{darwin::resource::initial_limits()};
  std::string login_name;
  std::uint32_t task_port{mach_task_identity::initial_task_self_name};
  std::uint32_t thread_port{mach_task_identity::initial_thread_self_name};
  std::uint32_t host_port{mach_task_identity::initial_host_self_name};
  std::uint32_t bootstrap_port{mach_task_identity::initial_bootstrap_name};
  std::uint32_t clock_port{mach_task_identity::initial_clock_name};
  std::uint32_t calendar_clock_port{
      mach_task_identity::initial_calendar_clock_name};
  std::uint32_t io_master_port{mach_task_identity::initial_io_master_name};
  std::uint32_t io_registry_options_port{
      mach_task_identity::initial_io_registry_options_name};
  std::int32_t thread_base_priority{xnu792::scheduler::default_base_priority};
  std::int32_t nice_value{};
  bool exited{};
  bool waiting_for_events{};
  std::uint32_t exit_status{};
  std::uint32_t termination_signal{};
};

struct KeventRegistration {
  std::uint32_t ident{};
  std::int16_t filter{};
  std::uint16_t flags{};
  std::uint32_t filter_flags{};
  std::int32_t data{};
  std::uint32_t user_data{};
  std::uint64_t process_exec_generation{};
  std::uint64_t process_exit_generation{};
};

struct PendingWait {
  std::int32_t target_pid{-1};
  std::uint32_t status_address{};
  std::uint32_t options{};
  std::size_t processor{};
};

struct PendingMachReceive {
  std::uint32_t message_address{};
  std::uint32_t receive_size{};
  std::uint32_t receive_name{};
  std::uint32_t options{};
  std::size_t processor{};
  std::optional<std::uint64_t> deadline;
  // mach_msg resolves the task-local name when the receive starts, then
  // blocks on the ipc object. Cache that object after the first resolution so
  // scheduler polling does not repeatedly walk the task namespace.
  std::optional<std::uint32_t> receive_object;
  // The shared queue generation observed while this receive last found no
  // message. A new enqueue or a port-set addition advances the generation,
  // while an unchanged value proves another locked tree walk cannot succeed.
  std::uint64_t observed_queue_generation{};
};

struct PendingKevent {
  std::uint32_t queue_fd{};
  std::uint32_t event_address{};
  std::uint32_t event_count{};
  std::size_t processor{};
  std::optional<std::uint64_t> deadline;
};

struct PendingRecvmsg {
  std::uint32_t fd{};
  std::uint32_t message_address{};
  std::size_t processor{};
};

struct PendingSocketRead {
  std::uint32_t fd{};
  std::uint32_t address{};
  std::uint32_t size{};
  std::uint32_t source_address{};
  std::uint32_t source_length_address{};
  std::size_t processor{};
  std::optional<std::uint64_t> deadline;
};

struct PendingHostConnect {
  std::uint32_t fd{};
  std::size_t processor{};
};

struct PendingHostAccept {
  std::uint32_t fd{};
  std::uint32_t address{};
  std::uint32_t length_address{};
  std::size_t processor{};
};

struct PendingHostWrite {
  std::uint32_t fd{};
  std::shared_ptr<HostSocket> socket;
  std::vector<std::byte> bytes;
  std::vector<std::byte> destination;
};

struct PendingUnixAccept {
  std::uint32_t fd{};
  std::uint32_t address{};
  std::uint32_t length_address{};
  std::size_t processor{};
};

struct PendingFlock {
  std::uint32_t fd{};
  bsd::AdvisoryLockKind kind{bsd::AdvisoryLockKind::Shared};
  std::shared_ptr<bsd::RegularFileOpenDescription> description;
  std::size_t processor{};
};

struct PendingRecordLock {
  std::uint32_t fd{};
  std::uint32_t permanent_file_id{};
  bsd::RecordLockRange range;
  std::size_t processor{};
};

struct PendingSelect {
  std::uint32_t descriptor_count{};
  std::uint32_t read_address{};
  std::uint32_t write_address{};
  std::uint32_t exception_address{};
  std::vector<std::uint32_t> read_words;
  std::vector<std::uint32_t> write_words;
  std::size_t processor{};
  std::optional<std::uint64_t> deadline;
};

struct PendingSemaphoreWait {
  std::uint32_t semaphore{};
  std::size_t processor{};
  std::optional<std::uint64_t> deadline;
  bool bsd_result{};
};

enum class PendingTimerKind {
  MachWaitUntil,
  ThreadSwitch,
  ClockSleep,
};

struct PendingTimer {
  struct BootstrapRetry {
    std::string service_name;
    std::uint64_t observed_generation{};
  };

  std::uint64_t deadline{};
  PendingTimerKind kind{PendingTimerKind::MachWaitUntil};
  std::optional<std::uint32_t> wakeup_time_address;
  bool calendar_clock{};
  std::optional<BootstrapRetry> bootstrap_retry;
};

// A local stream endpoint is an open file description, not an fd.  dup(2),
// fork(2), and SCM_RIGHTS all retain the same description; the peer observes
// close/EOF only after the final reference has gone away.
struct SocketPairLifetime {
  std::array<std::atomic_bool, 2> read_open{true, true};
  std::array<std::atomic_bool, 2> write_open{true, true};
  // Absolute receive positions are protected by KernelSharedState's socket
  // mutex. They bind SOCK_STREAM ancillary records to the byte that carried
  // them even while ordinary and recvmsg reads are interleaved.
  std::array<std::uint64_t, 2> read_offsets{};
};

struct SocketPairOpenDescription {
  std::shared_ptr<SocketPairLifetime> lifetime;
  std::uint32_t side{};

  SocketPairOpenDescription(std::shared_ptr<SocketPairLifetime> pair_lifetime,
                            std::uint32_t endpoint_side)
      : lifetime{std::move(pair_lifetime)}, side{endpoint_side} {}
  SocketPairOpenDescription(const SocketPairOpenDescription &) = delete;
  SocketPairOpenDescription &
  operator=(const SocketPairOpenDescription &) = delete;

  ~SocketPairOpenDescription() {
    if (!lifetime || side >= lifetime->read_open.size())
      return;
    lifetime->read_open[side].store(false, std::memory_order_release);
    lifetime->write_open[side].store(false, std::memory_order_release);
  }
};

struct SocketPairEndpoint {
  std::uint32_t pair{};
  std::uint32_t side{};
  std::shared_ptr<SocketPairOpenDescription> description;

  [[nodiscard]] bool local_read_open() const {
    return description && description->lifetime &&
           description->lifetime->read_open[side].load(
               std::memory_order_acquire);
  }
  [[nodiscard]] bool local_write_open() const {
    return description && description->lifetime &&
           description->lifetime->write_open[side].load(
               std::memory_order_acquire);
  }
  [[nodiscard]] bool peer_read_open() const {
    return description && description->lifetime &&
           description->lifetime->read_open[1U - side].load(
               std::memory_order_acquire);
  }
  [[nodiscard]] bool peer_write_open() const {
    return description && description->lifetime &&
           description->lifetime->write_open[1U - side].load(
               std::memory_order_acquire);
  }
  void shutdown_read() const {
    if (description && description->lifetime) {
      description->lifetime->read_open[side].store(false,
                                                   std::memory_order_release);
    }
  }
  void shutdown_write() const {
    if (description && description->lifetime) {
      description->lifetime->write_open[side].store(false,
                                                    std::memory_order_release);
    }
  }
};

[[nodiscard]] inline std::pair<SocketPairEndpoint, SocketPairEndpoint>
make_socket_pair_endpoints(std::uint32_t pair) {
  auto lifetime = std::make_shared<SocketPairLifetime>();
  return {
      SocketPairEndpoint{
          pair, 0, std::make_shared<SocketPairOpenDescription>(lifetime, 0)},
      SocketPairEndpoint{
          pair, 1,
          std::make_shared<SocketPairOpenDescription>(std::move(lifetime), 1)}};
}

struct KernelSharedState {
  std::string device_product_type;
  std::string device_board_config;
  std::string device_model_number;
  std::uint64_t device_ram_bytes{};

  struct NetworkInterface {
    std::uint16_t flags{};
    std::uint16_t index{};
    std::uint32_t family{};
    std::uint32_t unit{};
    std::uint32_t mtu{};
    std::uint8_t type{};
    std::array<std::byte, 6> link_address{};
    std::uint8_t link_address_length{};
    bool has_ipv4{};
    bool has_ipv6{};
    std::array<std::byte, 16> ipv4_address{};
    std::array<std::byte, 16> ipv4_netmask{};
    std::array<std::byte, 16> ipv4_broadcast{};
    std::array<std::byte, 16> ipv4_gateway{};
    std::array<std::byte, 28> ipv6_address{};
    std::array<std::byte, 28> ipv6_netmask{};
  };
  struct KernelEvent {
    std::uint32_t identifier{};
    std::uint32_t vendor{};
    std::uint32_t event_class{};
    std::uint32_t event_subclass{};
    std::uint32_t event_code{};
    std::vector<std::byte> bytes;
  };
  struct RouteSocketMessage {
    std::uint32_t identifier{};
    std::vector<std::byte> bytes;
    std::uint8_t family{};
    std::optional<std::uint64_t> receiver_socket;
  };
  struct RouteSocketState {
    std::uint64_t identifier{};
    std::uint32_t next_message_identifier{};
    std::uint32_t protocol{};
  };
  struct MountEntry {
    std::string type;
    std::string path;
    std::string source;
    std::uint32_t flags{};
  };
  struct MachMessage {
    enum class GraphicsInputKind {
      None,
      Touch,
      Home,
      Lock,
      OtherSystem,
    };
    struct ReceivePointerFixup {
      std::uint32_t value_offset{};
      std::uint32_t target_offset{};
    };
    struct OolPayload {
      std::uint32_t descriptor_offset{};
      std::vector<std::byte> bytes;
    };
    struct PortTransfer {
      std::uint32_t descriptor_offset{};
      std::uint32_t sender_name{};
      std::optional<std::uint32_t> array_index;
      std::uint32_t object{};
      xnu792::ipc::Right right{xnu792::ipc::Right::Send};
      std::uint32_t disposition{};
    };
    struct OolPortArray {
      std::uint32_t descriptor_offset{};
      std::uint32_t count{};
    };

    std::vector<std::byte> bytes;
    std::uint32_t destination{};
    std::uint32_t sender_pid{};
    std::uint32_t sender_uid{};
    std::uint32_t sender_gid{};
    std::uint64_t graphics_input_sequence{};
    GraphicsInputKind graphics_input_kind{GraphicsInputKind::None};
    std::optional<TouchPhase> graphics_touch_phase;
    std::vector<OolPayload> ool_payloads;
    std::vector<OolPortArray> ool_port_arrays;
    std::optional<std::uint32_t> reply_object;
    std::optional<xnu792::ipc::Right> reply_right;
    // A MOVE_SEND used as the message's remote port has no sender ipc_entry
    // after copyin, but the queued message still keeps the destination port
    // alive until receive/discard. Record that hold explicitly.
    std::optional<std::uint32_t> destination_send_object;
    std::vector<PortTransfer> port_transfers;
    std::vector<ReceivePointerFixup> receive_pointer_fixups;
  };
  struct ClockAlarm {
    std::uint64_t deadline{};
    std::uint64_t alarm_time{};
    std::uint32_t alarm_type{};
    std::uint32_t reply_object{};
  };
  struct UnixListener {
    std::uint32_t owner_pid{};
    std::uint32_t owner_fd{};
    std::deque<SocketPairEndpoint> pending_endpoints;
  };
  struct DescriptorTransfer {
    enum class Kind : std::uint8_t { File, Virtual };

    Kind kind{Kind::Virtual};
    std::filesystem::path file_path;
    std::uint64_t file_offset{};
    std::uint32_t file_status_flags{};
    std::shared_ptr<bsd::RegularFileOpenDescription>
        regular_file_open_description;
    std::optional<std::pair<std::uint32_t, bool>> block_device;
    std::string virtual_type;
    std::optional<SocketPairEndpoint> socket_endpoint;
    std::shared_ptr<UnixListener> unix_listener_state;
    std::shared_ptr<RouteSocketState> route_socket_state;
    std::shared_ptr<bsd::VirtualUdpSocket> virtual_udp_socket;
    std::string bound_name;
    bool listening{};
    std::vector<KeventRegistration> kqueue_registrations;
  };
  struct SocketAncillaryRecord {
    std::uint64_t byte_offset{};
    std::vector<DescriptorTransfer> transfers;
  };
  enum class GraphicsInputAbi {
    LegacyMouse,
    Darwin9_0,
    Darwin9_3,
  };
  struct ProcessRecord {
    std::uint32_t parent_pid{};
    std::uint32_t process_group{};
    std::uint32_t uid{};
    std::uint32_t effective_uid{};
    std::uint32_t gid{};
    std::uint32_t effective_gid{};
    std::uint32_t exit_status{};
    std::uint32_t termination_signal{};
    bool exited{};
    std::string command;
    std::string executable_path;
    std::vector<std::string> arguments;
    std::vector<std::string> environment;
    GraphicsInputAbi graphics_input_abi{GraphicsInputAbi::Darwin9_0};
  };
  struct ProcessKeventState {
    std::uint64_t exec_generation{};
    std::uint64_t exit_generation{};
    std::uint32_t wait_status{};
  };
  struct IOKitNotification {
    std::uint32_t owner_pid{};
    std::uint32_t notification_port{};
    std::string type;
    std::vector<std::byte> matching;
  };
  struct IOKitRegistryProperty {
    enum class Kind {
      String,
      Data,
      Boolean,
      Number,
    };

    Kind kind{Kind::Data};
    std::vector<std::byte> value;
  };
  enum class IOKitUserClientProfile {
    None,
    Generic,
    Display,
  };
  struct IOKitService {
    std::string class_name;
    std::vector<std::string> conforms_to;
    std::map<std::string, IOKitRegistryProperty> properties;
    std::string registry_path;
    std::uint32_t parent_object{};
    IOKitUserClientProfile user_client_profile{IOKitUserClientProfile::None};

    IOKitService() = default;
    IOKitService(std::string service_class,
                 std::vector<std::string> service_conformance,
                 std::map<std::string, IOKitRegistryProperty>
                     registry_properties = {},
                 std::string service_registry_path = {},
                 std::uint32_t service_parent_object = 0,
                 IOKitUserClientProfile service_user_client_profile =
                     IOKitUserClientProfile::None)
        : class_name(std::move(service_class)),
          conforms_to(std::move(service_conformance)),
          properties(std::move(registry_properties)),
          registry_path(std::move(service_registry_path)),
          parent_object(service_parent_object),
          user_client_profile(service_user_client_profile) {}
  };
  struct IOKitConnection {
    std::uint32_t service_port{};
    std::uint32_t owner_pid{};
    std::uint32_t type{};
  };
  struct IOKitInterestNotification {
    std::uint32_t owner_pid{};
    std::uint32_t wake_port{};
    std::string type;
    std::vector<std::uint32_t> reference;
  };
  struct IOKitDisplayVSync {
    std::uint32_t owner_pid{};
    std::uint32_t notification_port{};
    std::uint32_t notification_type{};
    std::uint32_t registration_reference{};
    std::array<std::uint32_t, 8> async_reference{};
    std::optional<std::uint64_t> next_deadline;
    std::uint64_t sequence{};
    std::uint64_t method_call_count{};
    bool enabled{};
  };
  struct IOKitDisplayConnectionState {
    std::uint32_t requested_power_state{};
  };
  struct PendingGraphicsInput {
    enum class Kind {
      Touch,
      SystemEvent,
    };
    Kind kind{Kind::Touch};
    TouchInput touch;
    std::uint32_t system_event_type{};
    std::uint64_t input_sequence{};
    MachMessage::GraphicsInputKind input_kind{
        MachMessage::GraphicsInputKind::None};
  };
  struct PendingBootstrapServiceLookup {
    std::string service_name;
    std::uint32_t requester_process_id{};
    std::uint64_t origin_touch_sequence{};
    bool application_launch_candidate{};
  };
  enum class ApplicationSuspensionReason {
    None,
    Home,
    Lock,
  };
  enum class HostDisplayIntent {
    GuestControlled,
    LockedOff,
    WakePending,
  };
  enum class ApplicationLaunchOrigin {
    Spawn,
    EventServiceLookup,
  };
  enum class ApplicationLaunchPhase {
    Launching,
    Active,
    Suspended,
    InterruptedHome,
    HeldLock,
  };
  struct ApplicationLaunchAttempt {
    std::uint64_t token{};
    std::uint64_t origin_touch_sequence{};
    ApplicationLaunchOrigin origin{ApplicationLaunchOrigin::Spawn};
    ApplicationLaunchPhase phase{ApplicationLaunchPhase::Launching};
  };
  struct ApplicationLaunchBarrier {
    ApplicationSuspensionReason reason{ApplicationSuspensionReason::None};
    std::uint64_t input_sequence{};
  };
  struct HeldApplicationLaunch {
    std::uint64_t origin_touch_sequence{};
    std::uint64_t lock_input_sequence{};
    std::uint32_t process_id{};
    std::uint64_t launch_token{};
    std::uint64_t unlock_up_sequence{};
  };
  struct ApplicationTouchTransform {
    float presentation_offset_x{};
    float presentation_offset_y{};
    float screen_origin_y{};

    bool operator==(const ApplicationTouchTransform &) const = default;
  };
  struct PendingApplicationSceneTransform {
    std::uint32_t process_id{};
    ApplicationTouchTransform transform;
  };
  struct ActiveApplicationScene {
    std::uint32_t process_id{};
    std::uint32_t event_object{};
    std::optional<ApplicationTouchTransform> touch_transform;
  };
  struct MachSemaphore {
    std::int64_t count{};
    std::uint32_t owner_pid{};
    std::deque<std::pair<std::uint32_t, std::uint32_t>> waiters;
  };
  struct MachTimer {
    std::uint32_t owner_pid{};
    std::optional<std::uint64_t> deadline;
  };
  struct MachMemoryObject {
    std::vector<std::shared_ptr<GuestPageBacking>> pages;
  };
  struct MachMemoryEntry {
    std::shared_ptr<MachMemoryObject> object;
    std::size_t first_page{};
    std::uint64_t size{};
    std::uint32_t protection{};
    bool purgable{};
  };
  struct TaskExceptionAction {
    std::uint32_t port_object{};
    std::uint32_t behavior{};
    std::uint32_t flavor{};

    bool operator==(const TaskExceptionAction &) const = default;
  };
  static constexpr std::size_t task_exception_type_count = 11;
  using TaskExceptionActions =
      std::array<TaskExceptionAction, task_exception_type_count>;
  struct MachNotificationRequest {
    std::uint32_t notify_object{};
    std::uint32_t sync{};
  };
  struct MachDeadNameNotificationRequest {
    std::uint32_t target_object{};
    std::uint32_t notify_object{};
    std::uint32_t sync{};
  };
  // The caller must hold mach_mutex. This allocates a global ipc_port object
  // identifier, never a task-local Mach name. The stride keeps synthetic
  // object identifiers distinct from the fixed early-boot objects while
  // task-local names remain exclusively owned by MachNamespaceTable.
  static constexpr std::uint32_t first_synthetic_mach_object = 0x10000U;
  static constexpr std::uint32_t synthetic_mach_object_stride = 0x100U;
  [[nodiscard]] std::uint32_t allocate_mach_object() {
    const auto object = next_mach_object;
    next_mach_object += synthetic_mach_object_stride;
    return object;
  }

  std::uint32_t desired_vnodes{65'536};
  std::string hostname{"localhost"};
  std::array<std::uint32_t, 2> task_for_pid_groups{};
  mutable std::mutex network_mutex;
  std::map<std::string, NetworkInterface> network_interfaces{
      {"lo0",
       {darwin::network::interface_flag_loopback |
            darwin::network::interface_flag_running,
        1,
        darwin::network::interface_family_loopback,
        0,
        darwin::network::default_loopback_mtu,
        darwin::network::interface_type_loopback,
        {},
        0}},
      {"en0",
       {darwin::network::interface_flag_broadcast |
            darwin::network::interface_flag_simplex |
            darwin::network::interface_flag_multicast,
        2,
        darwin::network::interface_family_ethernet,
        0,
        darwin::network::default_ethernet_mtu,
        darwin::network::interface_type_ethernet,
        virtual_network::interface_mac_address,
        6}},
  };
  std::uint32_t next_kernel_event_identifier{1};
  std::deque<KernelEvent> kernel_events;
  darwin::route::Table route_table;
  mutable std::mutex route_socket_mutex;
  std::uint32_t next_route_message_identifier{1};
  std::uint64_t next_route_socket_identifier{1};
  std::deque<RouteSocketMessage> route_socket_messages;
  std::vector<MountEntry> mounts{{"hfs", "/", "/dev/disk0s1", 0x00005001U}};
  std::vector<std::byte> nvram_serialized;
  std::uint32_t next_mach_object{first_synthetic_mach_object};
  std::uint32_t default_processor_set_name_object{};
  std::uint32_t default_processor_set_control_object{};
  xnu792::ipc::MachNamespaceTable mach_namespaces;
  // Global ipc_port objects. Per-task names and rights live exclusively in
  // MachNamespaceTable and resolve to keys in this table.
  xnu792::ipc::PortObjectTable mach_port_objects;
  // Send rights captured by queued messages count as extant for no-senders
  // even though they no longer need a sender-local ipc_entry.
  std::map<std::uint32_t, std::uint32_t> mach_inflight_send_rights;
  // Kernel-owned task metadata (for example task special ports) retains a
  // Send-like reference independently of every guest ipc_space. Keep that
  // reference visible to no-senders and transient-object reclaimers.
  std::map<std::uint32_t, std::uint32_t> mach_kernel_send_rights;
  // Port teardown can discard a queued right that points back to the same
  // object. Keep removal idempotent while that recursive ipc_right path is
  // unwinding; otherwise the queue sidecar is visited after its owner has
  // already been moved out and freed.
  std::set<std::uint32_t> mach_ports_being_removed;
  std::map<std::uint32_t, std::vector<std::uint32_t>> mach_port_sets;
  // These maps are keyed by global IPC object identifiers, never by a
  // caller's task-local Mach name. Keep task identity separate from generic
  // receive ownership so pid_for_task cannot mistake a service for a task.
  std::map<std::uint32_t, std::uint32_t> task_port_pids;
  // XNU exposes itk_nself as a distinct, read-only task-name capability.
  // Keeping it separate prevents task_name_for_pid from accidentally
  // granting task-control MIG operations through an identity-only port.
  std::map<std::uint32_t, std::uint32_t> task_name_port_pids;
  // Global thread-port objects indexed by task PID and that task's logical
  // thread slot. Task-local names are produced only when a caller receives
  // task_threads(), preserving ipc_space separation.
  std::map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>>
      task_thread_port_objects;
  std::map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>>
      task_special_ports;
  // XNU stores exception actions on the task object, not in the caller's
  // task-local IPC namespace. Port fields therefore use global IPC objects.
  std::map<std::uint32_t, TaskExceptionActions> task_exception_actions;
  std::map<std::uint32_t, std::deque<MachMessage>> mach_queues;
  // Queue producers may run on host input/device threads while guest kernels
  // poll from the scheduler. Keep the cheap readiness snapshot lock-free;
  // the queue and every generation transition remain serialized by
  // mach_mutex.
  std::atomic_uint64_t mach_queue_generation{1};

  // The caller holds mach_mutex. Centralizing enqueue makes it impossible for
  // a new message source to publish data without also waking cached empty
  // receives on the next scheduler pass.
  void enqueue_mach_message_locked(std::uint32_t destination,
                                   MachMessage message) {
    mach_queues[destination].push_back(std::move(message));
    mach_queue_generation.fetch_add(1, std::memory_order_release);
  }

  // Adding a populated port to a set can make an already queued message
  // newly visible without enqueueing another message.
  void note_mach_queue_topology_change_locked() {
    mach_queue_generation.fetch_add(1, std::memory_order_release);
  }

  [[nodiscard]] std::uint64_t mach_queue_generation_snapshot() const {
    return mach_queue_generation.load(std::memory_order_acquire);
  }
  // launchd remains the authority for the bootstrap namespace. These caches
  // only remember replies already observed on the emulated Mach IPC path so
  // host devices can address the same global ipc_port objects.
  // A Mach reply port can carry more than one outstanding bootstrap lookup.
  // Replies preserve queue order, so retain every request instead of letting
  // a later lookup overwrite the metadata needed to classify an earlier one.
  std::map<std::uint32_t, std::deque<PendingBootstrapServiceLookup>>
      pending_bootstrap_service_lookups;
  std::map<std::string, std::uint32_t> bootstrap_service_objects;
  // A failed bootstrap lookup is commonly followed by a bounded polling
  // sleep while launchd starts an on-demand provider. Track the precise
  // service condition so registration can wake that retry without waiting
  // for an unrelated fixed timeout.
  std::map<std::uint32_t, PendingTimer::BootstrapRetry>
      pending_bootstrap_retries;
  std::map<std::string, std::uint64_t> bootstrap_service_generations;
  // A successful bootstrap_check_in is an observable guest boot milestone:
  // launchd accepted a service provider and transferred its receive right.
  // Keep names only to deduplicate retries; status reporting exposes a count
  // and does not depend on any firmware-specific service name.
  std::set<std::string> bootstrap_checked_in_services;
  // A Purple application registers a bootstrap service backed by its own
  // receive right. When SpringBoard resolves that service, retain the global
  // port object so host touch input can follow Purple's foreground routing.
  std::uint32_t pending_application_event_object{};
  std::uint32_t active_application_event_object{};
  bool application_touch_suspended{};
  ApplicationSuspensionReason application_suspension_reason{
      ApplicationSuspensionReason::None};
  std::optional<std::uint32_t> suspended_application_scene_process_id;
  std::uint64_t next_graphics_input_sequence{1};
  std::uint64_t springboard_last_consumed_touch_sequence{};
  // Launch causality belongs to the gesture that selected an icon, not its
  // final Up delivery. Home cancels that gesture; Lock only holds it until a
  // successful unlock.
  std::uint64_t springboard_active_touch_begin_sequence{};
  std::uint64_t springboard_last_touch_begin_sequence{};
  // At most one SpringBoard gesture may nominate the next foreground App.
  // Bootstrap lookups and spawn consume this exact sequence; historical
  // touches must never turn unrelated resident-service probes into launches.
  std::uint64_t springboard_pending_launch_touch_sequence{};
  // Input can be waiting in SpringBoard's Mach queue when a host Lock/Home
  // command arrives. Keep the host-enqueue gesture boundary as well as the
  // receive-side boundary above so a loaded guest cannot lose launch
  // causality merely because it has not drained the touch message yet.
  std::uint64_t springboard_enqueued_active_touch_begin_sequence{};
  std::uint64_t springboard_enqueued_last_touch_begin_sequence{};
  std::uint64_t springboard_enqueued_last_touch_end_sequence{};
  // The first SpringBoard-directed gesture after Home wakes a locked display
  // is the unlock gesture, not an application-launch intent. Retain its exact
  // sequence range so service lookups caused by unlock cannot revive a held
  // launch attempt.
  bool springboard_unlock_touch_pending{};
  bool springboard_unlock_touch_active{};
  std::uint64_t springboard_unlock_touch_begin_sequence{};
  std::uint64_t springboard_unlock_touch_end_sequence{};
  float springboard_unlock_touch_start_x{};
  float springboard_unlock_touch_start_y{};
  std::uint64_t next_application_launch_token{1};
  std::optional<ApplicationLaunchBarrier> application_launch_barrier;
  // Home is a cancellation watermark independent of the latest Lock barrier.
  // Keeping it monotonic prevents a later Lock from reviving an older launch.
  std::uint64_t last_home_launch_barrier_sequence{};
  // Every attempt is bound to an exact PID and a reliable SpringBoard target
  // event. Scene/lifecycle callbacks may observe an attempt but never create
  // one, so an old callback cannot consume a newer foreground intent.
  std::map<std::uint32_t, ApplicationLaunchAttempt>
      application_launch_attempts;
  std::optional<std::uint32_t>
      foreground_application_attempt_process_id;
  // A Lock holds one exact foreground launch. The token may initially contain
  // only the selecting gesture; a later spawn or service lookup binds its PID.
  // Unlock records completion but activation remains owned by the firmware's
  // subsequent lifecycle/scene commit.
  std::optional<HeldApplicationLaunch> held_application_launch;
  // Lock can preempt an already-running Home exit after SpringBoard has
  // committed only a partial desktop transform. A deliberate unlock then
  // requests one final Home redraw, even if the outgoing App has exited.
  std::optional<std::uint64_t>
      interrupted_home_exit_lock_sequence;
  // CoreSurface publication sequences are immutable even if a transport ID
  // or PID is later reused. Track only full-screen application publications:
  // SpringBoard's home-screen icons also retain application provenance and
  // must never be hidden merely because that application was interrupted.
  std::map<std::uint32_t, std::set<std::uint64_t>>
      application_fullscreen_surface_publications;
  // While a launch is causally interrupted, new full-screen publications
  // from that producer are suppressed as well as the ones already known.
  std::set<std::uint32_t>
      suppress_future_application_fullscreen_surface_processes;
  std::set<std::pair<std::uint32_t, std::uint64_t>>
      suppressed_application_fullscreen_surface_publications;
  std::atomic_bool application_fullscreen_surface_suppression_active{false};
  // Host Lock must win over a late userspace power-on. Conversely, once a
  // wake begins, the trailing panel-off request from the preceding Lock must
  // not turn the newly woken display black. Keep the power transaction
  // separate from the guest-visible Home and Lock events.
  HostDisplayIntent host_display_intent{HostDisplayIntent::GuestControlled};
  // Each Lock Down starts one asynchronous SpringBoard panel-off request.
  // Keep all unresolved generations: under load an older request can arrive
  // several seconds after a later wake/lock cycle has already begun.
  std::deque<std::uint64_t>
      host_display_pending_lock_power_off_sequences;
  std::uint64_t host_display_current_lock_down_sequence{};
  std::uint64_t host_display_wake_after_lock_sequence{};
  bool host_display_wake_power_on_acknowledged{};
  // Physical Home and Sleep/Wake own the hardware power domain. They may
  // reveal the retained lock scene before SpringBoard catches up, but never
  // change the identity of the event delivered to the guest.
  bool host_display_hardware_wake_pending{};
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t>
      application_scene_context_owners;
  std::map<std::uint32_t, ApplicationTouchTransform>
      application_scene_transforms;
  std::optional<PendingApplicationSceneTransform>
      latest_application_scene_transform;
  std::optional<ActiveApplicationScene> active_application_scene;
  // A normal App-to-App handoff can deliver willResignActive before
  // SpringBoard spawns the replacement. Retain that one process identity
  // across the route teardown so the replacement spawn remains a foreground
  // intent without borrowing an unrelated touch sequence.
  std::optional<std::uint32_t> pending_application_handoff_process_id;
  // First-generation SpringBoard prewarms selected applications with the
  // userspace `--suspended` argument. Their first activation message binds the
  // remote event/scene plumbing but does not put that scene on the visual
  // foreground stack. A later activation is a real foreground transition.
  std::set<std::uint32_t> consumed_application_prewarm_activations;
  // SpringBoard alert items are system-owned windows layered above any remote
  // application scene. Track object identity so nested/repeated activation
  // callbacks cannot restore application input prematurely.
  std::set<std::uint32_t> active_springboard_alert_items;
  std::deque<PendingGraphicsInput> pending_graphics_inputs;
  std::map<std::uint32_t, MachSemaphore> mach_semaphores;
  std::map<std::uint32_t, MachTimer> mach_timers;
  // XNU named-memory entries are kernel ipc_port objects. The per-task Mach
  // namespace carries rights; this table carries the referenced VM object.
  std::map<std::uint32_t, MachMemoryEntry> mach_memory_entries;
  std::map<std::uint64_t, ClockAlarm> clock_alarms;
  std::uint64_t next_clock_alarm{1};
  std::map<std::pair<std::uint32_t, std::uint32_t>, MachNotificationRequest>
      mach_notifications;
  std::map<std::pair<std::uint32_t, std::uint32_t>,
           MachDeadNameNotificationRequest>
      mach_dead_name_notifications;
  std::set<std::pair<std::uint32_t, std::uint32_t>> semaphore_wakeups;
  // Destruction wakes semaphore waiters with KERN_TERMINATED rather than as a
  // successful signal. Keep that result distinct from ordinary wakeups while
  // the owning CompatibilityKernel instances finish their blocked traps.
  std::set<std::pair<std::uint32_t, std::uint32_t>> semaphore_terminations;
  std::map<std::uint32_t, std::deque<std::uint32_t>> iokit_iterators;
  std::map<std::uint32_t, IOKitService> iokit_services;
  std::uint32_t iokit_registry_root_object{
      mach_task_identity::initial_io_registry_options_name};
  std::map<std::uint32_t, IOKitConnection> iokit_connections;
  std::map<std::uint32_t, IOKitInterestNotification>
      iokit_interest_notifications;
  // Keyed by the global IOUserClient connection object. The notification
  // port is also a global ipc_port object, never a task-local Mach name.
  std::map<std::uint32_t, IOKitDisplayVSync> iokit_display_vsync;
  std::map<std::uint32_t, IOKitDisplayConnectionState>
      iokit_display_connections;
  // The physical panel has one power state even though GraphicsServices and
  // LayerKit open separate AppleH1CLCD user clients.
  DisplayGeometry display_geometry{default_display_geometry};
  DisplayGeometry user_interface_geometry{default_display_geometry};
  std::optional<std::uint32_t> requested_display_power_state;
  std::uint32_t baseband_service{};
  bsd::baseband_device::State baseband_device_state;
  std::uint32_t mobile_framebuffer_service{};
  bool wifi_service_available{};
  std::uint32_t wifi_service{};
  std::uint32_t wifi_interface_service{};
  std::vector<IOKitNotification> iokit_notifications;
  VirtualClock clock;
  std::uint32_t next_socket_pair{1};
  std::map<std::uint32_t, std::array<std::deque<std::byte>, 2>>
      socket_pair_buffers;
  std::map<std::uint32_t, std::array<std::deque<SocketAncillaryRecord>, 2>>
      socket_pair_ancillary;
  std::shared_ptr<bsd::VirtualUdpNetwork> virtual_udp_network{
      std::make_shared<bsd::VirtualUdpNetwork>()};
  // A pathname is a registry entry, not an owner. The listening open file
  // description is retained by duplicated/inherited/transferred guest fds.
  std::map<std::string, std::weak_ptr<UnixListener>> unix_listeners;
  // bind(2) creates an AF_UNIX namespace node. Closing the socket does not
  // unlink that node; the guest must remove it explicitly, just as on XNU.
  std::set<std::string> unix_socket_nodes;
  std::uint32_t next_shared_memory_object{1};
  std::map<std::string, std::filesystem::path> shared_memory_objects;
  // File-backed MAP_SHARED mappings use one physical page cache across tasks.
  // The host file is only the initial pager source; guest stores remain in the
  // simulator's shared memory object and become immediately visible through
  // every mapping of the same file identity.
  std::shared_ptr<FilePageCache> shared_mapping_page_cache{
      std::make_shared<FilePageCache>()};
  // Hard links have distinct catalog IDs but share one HFS file record.
  // Metadata mutations therefore follow the permanent inode identity.
  std::map<std::uint32_t, hfs::MetadataOverride> hfs_metadata_overrides;
  // A disengaged value is a guest-side removal tombstone hiding an
  // attribute preserved in the immutable extracted firmware tree.
  std::map<std::uint32_t,
           std::map<std::string, std::optional<std::vector<std::byte>>>>
      hfs_named_attribute_overrides;
  std::map<std::uint32_t, ProcessRecord> processes;
  // EVFILT_PROC is edge-triggered and may outlive the process-table zombie.
  // Retain compact per-PID generations so an exec/exit between kevent
  // registration and the next scheduler poll cannot be lost.
  std::map<std::uint32_t, ProcessKeventState> process_kevent_states;
  std::shared_ptr<bsd::AdvisoryFileLockRegistry> advisory_file_locks{
      std::make_shared<bsd::AdvisoryFileLockRegistry>()};
  std::mutex mach_mutex;
  mutable std::mutex socket_mutex;
  mutable std::mutex filesystem_mutex;
};

// The caller must hold mach_mutex. A remote application may own cached scene
// state while suspended, and a prelaunched application may publish a scene
// before SpringBoard promotes its event port. Only the intersection with a
// currently active semantic scene is the visible App allowed to write the
// panel. A disengaged migrated_scene_committed retains the legacy transform
// fallback.
[[nodiscard]] inline bool active_application_owns_display_locked(
    const KernelSharedState &state, std::uint32_t process_id,
    std::optional<bool> migrated_scene_committed = std::nullopt) {
  // The outgoing App remains sampleable as a texture for SpringBoard's native
  // shrink animation, but suspension immediately revokes direct panel
  // ownership. Conflating the two lets a late first App frame flash
  // fullscreen after Home.
  if (state.application_touch_suspended ||
      state.active_application_event_object == 0U ||
      !state.active_application_scene ||
      state.active_application_scene->process_id != process_id ||
      !migrated_scene_committed.value_or(
          state.active_application_scene->touch_transform.has_value())) {
    return false;
  }
  const auto event_port =
      state.mach_port_objects.lookup(state.active_application_event_object);
  return event_port && event_port->receive_owner == process_id;
}

} // namespace ilemu
