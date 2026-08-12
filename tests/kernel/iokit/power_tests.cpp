#include "ilemu/address_space.hpp"
#include "ilemu/device_mig_ids.hpp"
#include "ilemu/iokit_abi.hpp"
#include "ilemu/kernel_iokit.hpp"
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
#include <string_view>

namespace ilemu::test::kernel {
namespace {

namespace device_mig = xnu792::mig::device;

void register_for_system_power_test() {
  AddressSpace memory;
  constexpr std::uint32_t message = 0x62000;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "IOKit power message map failed");

  KernelSharedState shared;
  ProcessContext process;
  process.pid = 19;
  shared.mach_namespaces.create_task(process.pid);
  const auto send_type = xnu792::ipc::type_mask(xnu792::ipc::Right::Send);
  for (const auto special :
       {process.io_registry_options_port, process.task_port}) {
    require(shared.mach_namespaces.install(process.pid, special, special,
                                           send_type),
            "IOKit power special-port installation failed");
  }
  std::ostringstream stream;
  Output output{stream};

  require(memory.write32(
              message + darwin::mig_wire::complex_descriptor_count_offset,
              iokit_abi::service_open_extended::request_descriptor_count),
          "IOKit power open fixture setup failed");
  require(
      handle_iokit_mach_request(
          memory, output, shared, process,
          static_cast<std::uint32_t>(iokit_abi::Message::ServiceOpenExtended),
          message, 60, 60, process.io_registry_options_port,
          0x10101) == std::optional<std::uint32_t>{0},
      "IOKit root power-domain open failed");
  const auto connection_name =
      memory.read32(message + darwin::mig_wire::descriptor_name_offset(0));
  const auto connection_object =
      connection_name
          ? shared.mach_namespaces.resolve(process.pid, *connection_name)
          : std::nullopt;
  require(connection_name && *connection_name != 0 && connection_object &&
              shared.iokit_connections.contains(*connection_object) &&
              memory.read32(message +
                            iokit_abi::service_open_extended::result_offset) ==
                  std::optional<std::uint32_t>{iokit_abi::success},
          "IOKit power open did not return a live connection");

  constexpr std::uint32_t wake_name = 0x100fc;
  constexpr std::uint32_t wake_object = 0xbeef1000U;
  require(shared.mach_namespaces.install(process.pid, wake_name, wake_object,
                                         send_type),
          "IOKit power wake-port installation failed");
  constexpr std::string_view interest{"IOGeneralInterest\0", 18};
  std::array<std::byte, interest.size()> interest_bytes{};
  for (std::size_t index = 0; index < interest.size(); ++index)
    interest_bytes[index] = static_cast<std::byte>(interest[index]);

  std::array<std::uint32_t,
             device_mig::io_service_add_interest_notification_arguments.size()>
      counts{};
  counts[1] = static_cast<std::uint32_t>(interest.size());
  const auto type_layout = xnu792::mig::compute_wire_layout(
      device_mig::io_service_add_interest_notification_arguments,
      xnu792::mig::WireLayoutSide::Request, counts);
  require(type_layout.has_value(),
          "IOKit interest type layout computation failed");
  constexpr std::array<std::uint32_t, 4> reference{0x11111111U, 0x22222222U,
                                                   0x33333333U, 0x44444444U};
  counts[3] = static_cast<std::uint32_t>(reference.size());
  const auto layout = xnu792::mig::compute_wire_layout(
      device_mig::io_service_add_interest_notification_arguments,
      xnu792::mig::WireLayoutSide::Request, counts);
  require(layout.has_value(), "IOKit interest layout computation failed");
  require(
      memory.write32(
          message + darwin::mig_wire::complex_descriptor_count_offset,
          iokit_abi::service_interest_notification::request_descriptor_count) &&
          memory.write32(message + (*layout)[2].offset, wake_name) &&
          memory.write32(message + (*layout)[1].count_offset, counts[1]) &&
          memory.copy_in(message + (*layout)[1].offset, interest_bytes) &&
          memory.write32(message + (*type_layout)[3].count_offset, counts[3]),
      "IOKit interest fixture setup failed");
  for (std::size_t index = 0; index < reference.size(); ++index) {
    require(memory.write32(
                message + (*layout)[3].offset +
                    static_cast<std::uint32_t>(index * sizeof(std::uint32_t)),
                reference[index]),
            "IOKit interest reference write failed");
  }

  require(handle_iokit_mach_request(
              memory, output, shared, process,
              static_cast<std::uint32_t>(
                  iokit_abi::Message::ServiceAddInterestNotification),
              message, iokit_abi::service_interest_notification::reply_size,
              iokit_abi::service_interest_notification::reply_size,
              process.io_registry_options_port,
              0x10102) == std::optional<std::uint32_t>{0},
          "IOKit power interest registration failed");
  const auto notifier_name =
      memory.read32(message + darwin::mig_wire::descriptor_name_offset(0));
  const auto notifier_object =
      notifier_name
          ? shared.mach_namespaces.resolve(process.pid, *notifier_name)
          : std::nullopt;
  const auto notification =
      notifier_object
          ? shared.iokit_interest_notifications.find(*notifier_object)
          : shared.iokit_interest_notifications.end();
  require(notifier_name && *notifier_name != 0 && notifier_object &&
              notification != shared.iokit_interest_notifications.end() &&
              notification->second.wake_port == wake_object &&
              notification->second.type == "IOGeneralInterest" &&
              notification->second.reference.size() == reference.size(),
          "IOKit power interest state was not retained");
}

} // namespace

void run_iokit_power_tests() { register_for_system_power_test(); }

} // namespace ilemu::test::kernel
