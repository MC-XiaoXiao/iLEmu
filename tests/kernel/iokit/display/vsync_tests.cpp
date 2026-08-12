#include "ilemu/address_space.hpp"
#include "ilemu/device_mig_ids.hpp"
#include "ilemu/iokit_abi.hpp"
#include "ilemu/kernel_iokit.hpp"
#include "ilemu/kernel_iokit_display.hpp"
#include "ilemu/kernel_mach_ipc.hpp"
#include "ilemu/kernel_shared_state.hpp"
#include "ilemu/mach_namespace.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/output.hpp"
#include "ilemu/xnu_mig_adapter.hpp"

#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>

namespace ilemu::test::kernel {
namespace {

namespace device_mig = xnu792::mig::device;

std::uint32_t read_word(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
    value |= std::to_integer<std::uint32_t>(bytes[offset + byte])
             << (byte * 8U);
  }
  return value;
}

void display_vsync_notification_test() {
  AddressSpace memory;
  constexpr std::uint32_t message_address = 0x68000U;
  require(memory.map(message_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "display VSync message map failed");

  KernelSharedState state;
  ProcessContext process;
  process.pid = 31;
  state.mach_namespaces.create_task(process.pid);

  constexpr std::uint32_t service_object = 0xcafe0000U;
  constexpr std::uint32_t connection_object = 0xcafe0100U;
  constexpr std::uint32_t connection_name = 0x103U;
  constexpr std::uint32_t notification_object = 0xcafe0200U;
  constexpr std::uint32_t notification_name = 0x203U;
  const auto send_type = xnu792::ipc::type_mask(xnu792::ipc::Right::Send);
  const auto receive_type = xnu792::ipc::type_mask(xnu792::ipc::Right::Receive);
  require(state.mach_port_objects.create(service_object) &&
              state.mach_port_objects.create(connection_object) &&
              state.mach_port_objects.create(notification_object) &&
              state.mach_namespaces.install(process.pid, connection_name,
                                            connection_object, send_type) &&
              state.mach_namespaces.install(process.pid, notification_name,
                                            notification_object, receive_type),
          "display VSync Mach namespace setup failed");
  state.mach_queues.try_emplace(notification_object);
  state.iokit_services.emplace(
      service_object,
      KernelSharedState::IOKitService{"AppleH1CLCD", {"IOMobileFramebuffer"}});
  state.iokit_connections.emplace(
      connection_object,
      KernelSharedState::IOKitConnection{service_object, process.pid,
                                         iokit_abi::apple_h1clcd_service_type});

  constexpr std::uint32_t request_size = 56U;
  constexpr std::uint32_t reply_port = 0x303U;
  require(
      memory.write32(message_address + darwin::mig_wire::header_size_offset,
                     request_size) &&
          memory.write32(message_address +
                             darwin::mig_wire::complex_descriptor_count_offset,
                         1) &&
          memory.write32(message_address +
                             darwin::mig_wire::descriptor_name_offset(0),
                         notification_name) &&
          memory.write32(message_address +
                             darwin::mig_wire::descriptor_metadata_offset(0),
                         darwin::mig_wire::port_descriptor_metadata(
                             darwin::mig_wire::disposition_make_send)) &&
          memory.write32(
              message_address +
                  device_mig::io_connect_set_notification_port_arguments[1]
                      .request_offset,
              0) &&
          memory.write32(
              message_address +
                  device_mig::io_connect_set_notification_port_arguments[3]
                      .request_offset,
              0),
      "display VSync notification-port request setup failed");

  std::ostringstream stream;
  Output output{stream};
  require(
      handle_iokit_mach_request(
          memory, output, state, process,
          device_mig::id(device_mig::Routine::io_connect_set_notification_port),
          message_address, request_size, request_size, connection_name,
          reply_port) == std::optional<std::uint32_t>{0} &&
          memory.read32(message_address +
                        darwin::mig_wire::message_header_size +
                        darwin::mig_wire::ndr_record_size) ==
              std::optional<std::uint32_t>{iokit_abi::success},
      "display notification port registration failed");

  constexpr std::uint32_t firmware_callout = 0x31bf9978U;
  constexpr std::uint32_t framebuffer_refcon = 0x51000000U;
  const std::array<std::uint64_t, 2> scalar_input{firmware_callout,
                                                  framebuffer_refcon};
  const auto method = kernel_iokit::display::dispatch_connect_method(
      state, process, connection_object,
      static_cast<std::uint32_t>(
          iokit_abi::AppleH1ClcdSelector::SetVSyncNotifications),
      scalar_input, {}, 0);
  const auto deadline =
      kernel_iokit::display::next_vsync_deadline_locked(state);
  require(method && method->return_code == iokit_abi::success && deadline,
          "AppleH1CLCD selector 9 did not arm VSync");

  const std::array<std::uint64_t, 2> disabled_input{0, 0};
  const auto disabled = kernel_iokit::display::dispatch_connect_method(
      state, process, connection_object,
      static_cast<std::uint32_t>(
          iokit_abi::AppleH1ClcdSelector::SetVSyncNotifications),
      disabled_input, {}, 0);
  const auto reenabled = kernel_iokit::display::dispatch_connect_method(
      state, process, connection_object,
      static_cast<std::uint32_t>(
          iokit_abi::AppleH1ClcdSelector::SetVSyncNotifications),
      scalar_input, {}, 0);
  require(disabled && disabled->return_code == iokit_abi::success &&
              reenabled && reenabled->return_code == iokit_abi::success &&
              kernel_iokit::display::next_vsync_deadline_locked(state) ==
                  deadline,
          "display registration churn moved the fixed VSync phase");

  constexpr std::uint64_t display_power_on = 1;
  const std::array<std::uint64_t, 1> power_input{display_power_on};
  for (const auto selector :
       {iokit_abi::AppleH1ClcdSelector::RequestPowerChangeV2,
        iokit_abi::AppleH1ClcdSelector::RequestPowerChangeV1,
        iokit_abi::AppleH1ClcdSelector::RequestPowerChange}) {
    const auto power = kernel_iokit::display::dispatch_connect_method(
        state, process, connection_object,
        static_cast<std::uint32_t>(selector), power_input, {}, 0);
    require(power && power->return_code == iokit_abi::success &&
                state.iokit_display_connections.at(connection_object)
                        .requested_power_state == display_power_on &&
                state.requested_display_power_state == display_power_on,
            "AppleH1CLCD power selector did not retain display power state");
  }

  kernel_iokit::display::deliver_due_vsync_locked(state, *deadline);
  const auto queue = state.mach_queues.find(notification_object);
  require(queue != state.mach_queues.end() && queue->second.size() == 1,
          "VSync did not produce exactly one pending notification");
  const auto &bytes = queue->second.front().bytes;
  constexpr std::size_t notification_header =
      darwin::mig_wire::message_header_size;
  constexpr std::size_t reference =
      notification_header + 2U * sizeof(std::uint32_t);
  constexpr std::size_t completion =
      reference +
      iokit_abi::display_vsync::async_reference_count * sizeof(std::uint32_t);
  constexpr std::size_t argument = completion + sizeof(std::uint32_t);
  require(
      read_word(bytes, darwin::mig_wire::header_identifier_offset) ==
              iokit_abi::display_vsync::message_identifier &&
          read_word(bytes, notification_header) ==
              (1U + iokit_abi::display_vsync::completion_argument_count) *
                  sizeof(std::uint32_t) &&
          read_word(bytes, notification_header + sizeof(std::uint32_t)) ==
              iokit_abi::display_vsync::async_completion_type &&
          read_word(bytes, reference +
                               iokit_abi::display_vsync::async_callout_index *
                                   sizeof(std::uint32_t)) == firmware_callout &&
          read_word(bytes,
                    reference + iokit_abi::display_vsync::async_refcon_index *
                                    sizeof(std::uint32_t)) ==
              framebuffer_refcon &&
          read_word(bytes, completion) == iokit_abi::success &&
          read_word(bytes, argument + 2U * sizeof(std::uint32_t)) ==
              static_cast<std::uint32_t>(*deadline) &&
          read_word(bytes, argument + 3U * sizeof(std::uint32_t)) ==
              static_cast<std::uint32_t>(*deadline >> 32U) &&
          read_word(bytes, argument + 4U * sizeof(std::uint32_t)) == 0 &&
          read_word(bytes, argument + 5U * sizeof(std::uint32_t)) == 0,
      "XNU 792 async VSync message ABI mismatch");

  // A second deadline must not flood the Mach queue while the first pulse is
  // still pending in the CFRunLoop source.
  const auto following_deadline =
      kernel_iokit::display::next_vsync_deadline_locked(state);
  require(following_deadline && *following_deadline > *deadline,
          "periodic VSync deadline did not advance");
  kernel_iokit::display::deliver_due_vsync_locked(state, *following_deadline);
  require(queue->second.size() == 1,
          "VSync accumulated duplicate pending notifications");
}

} // namespace

void run_iokit_display_tests() { display_vsync_notification_test(); }

} // namespace ilemu::test::kernel
