#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/xattr.h>

#include "ilemu/address_space.hpp"
#include "ilemu/clock_mig_ids.hpp"
#include "ilemu/clock_reply_mig_ids.hpp"
#include "ilemu/core_surface_abi.hpp"
#include "ilemu/core_surface_hle.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kqueue_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
#include "ilemu/device_mig_ids.hpp"
#include "ilemu/display.hpp"
#include "ilemu/dnssd_ipc_abi.hpp"
#include "ilemu/gdb_rsp.hpp"
#include "ilemu/gles_abi.hpp"
#include "ilemu/hfs_metadata.hpp"
#include "ilemu/host_network.hpp"
#include "ilemu/iokit_abi.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/kernel_iokit.hpp"
#include "ilemu/kernel_mach_ipc.hpp"
#include "ilemu/mach_clock_abi.hpp"
#include "ilemu/mach_namespace.hpp"
#include "ilemu/mach_port_mig_ids.hpp"
#include "ilemu/mach_port_object.hpp"
#include "ilemu/mach_scheduler_abi.hpp"
#include "ilemu/mach_thread_policy_abi.hpp"
#include "ilemu/macho.hpp"
#include "ilemu/mbx2d_abi.hpp"
#include "ilemu/mbx2d_hle.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/mobile_framebuffer_hle.hpp"
#include "ilemu/opengles_hle.hpp"
#include "ilemu/surface_store.hpp"
#include "ilemu/system_configuration_mig_ids.hpp"
#include "ilemu/userland_hle.hpp"
#include "ilemu/virtual_network.hpp"
#include "ilemu/wifi_state.hpp"
#include "ilemu/xnu_mig_adapter.hpp"
#include "ilemu/xnu_scheduler.hpp"

#include "test_support.hpp"

#include "suite.hpp"

namespace ilemu::test::network_suite {
namespace {

using namespace ::ilemu;
using ::ilemu::test::require;

void wifi_state_test() {
  WifiState wifi;
  require(!wifi.snapshot().powered && wifi.scan().empty(),
          "powered-off Wi-Fi exposed scan results");
  require(wifi.set_power(true), "virtual Wi-Fi did not power on");
  const auto access_points = wifi.scan();
  require(access_points.size() == 1 &&
              access_points.front().ssid == "iLEmu",
          "virtual Wi-Fi scan did not expose the compatibility network");
  require(wifi.associate("iLEmu"), "virtual Wi-Fi association failed");
  const auto configured = wifi.snapshot();
  require(configured.link_state == WifiLinkState::Configured &&
              configured.associated_access_point.has_value() &&
              configured.ipv4.has_value() &&
              configured.ipv4->address == virtual_network::client_address &&
              configured.ipv4->dns_servers.size() == 1,
          "virtual association did not complete DHCP/DNS state");
  require(wifi.set_power(false) && !wifi.snapshot().ipv4,
          "power-off did not remove the virtual lease");

  AddressSpace memory;
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};
  kernel.set_preferred_wifi_networks({"iLEmu"});
  kernel.set_host_network_policy(HostNetworkPolicy::Loopback);
  const auto connected = kernel.network_interface_snapshot("en0");
  const auto connected_routes = kernel.route_snapshot();
  require(kernel.wifi_snapshot().link_state == WifiLinkState::Configured &&
              connected && connected->ipv4_address.has_value() &&
              (connected->flags & darwin::network::interface_flag_up) != 0 &&
              (connected->flags & darwin::network::interface_flag_running) !=
                  0 &&
              connected_routes.size() == 2 &&
              std::ranges::all_of(connected_routes, [](const auto &route) {
                return route.interface_name == "en0" &&
                       route.origin ==
                           darwin::route::Entry::Origin::Interface;
              }),
          "enabled host networking did not configure virtual en0");
  kernel.set_host_network_policy(HostNetworkPolicy::Isolated);
  const auto isolated = kernel.network_interface_snapshot("en0");
  require(isolated && !isolated->ipv4_address.has_value() &&
              (isolated->flags & darwin::network::interface_flag_up) == 0 &&
              kernel.route_snapshot().empty(),
          "host isolation left the virtual Wi-Fi link connected");
}

void configd_network_ioctl_test() {
  using namespace darwin::network;
  AddressSpace memory;
  constexpr std::uint32_t request = 0x3f000;
  constexpr std::uint32_t media_list = request + 0x100;
  require(memory.map(request, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "configd network ioctl page failed to map");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  const auto create_control_socket = [&](std::uint32_t family) {
    cpu.registers()[0] = family;
    cpu.registers()[1] = 2; // SOCK_DGRAM
    cpu.registers()[2] = 0;
    cpu.registers()[12] = 97;
    kernel.dispatch(cpu, 0x80);
    require((cpu.cpsr() & (1U << 29U)) == 0,
            "configd control socket creation failed");
    return cpu.registers()[0];
  };
  const auto issue_ioctl = [&](std::uint32_t fd, std::uint32_t command) {
    cpu.registers()[0] = fd;
    cpu.registers()[1] = command;
    cpu.registers()[2] = request;
    cpu.registers()[12] = 54;
    kernel.dispatch(cpu, 0x80);
  };
  const auto write_interface_name = [&] {
    std::array<std::byte, interface_name_size> name{};
    name[0] = std::byte{'e'};
    name[1] = std::byte{'n'};
    name[2] = std::byte{'0'};
    require(memory.copy_in(request, name),
            "configd interface name write failed");
  };

  const auto ipv4_fd = create_control_socket(address_family_inet);
  const auto ipv6_fd = create_control_socket(address_family_inet6);
  kernel.set_preferred_wifi_networks({"iLEmu"});
  kernel.set_host_network_policy(HostNetworkPolicy::Loopback);
  write_interface_name();
  constexpr std::uint32_t arm32_ifmediareq_size = 40;
  const auto get_media_command =
      ioctl_get_interface_media | (arm32_ifmediareq_size << 16U);
  issue_ioctl(ipv4_fd, get_media_command);
  const auto active_media =
      media_type_ethernet | media_subtype_100_tx | media_option_full_duplex;
  require(cpu.registers()[0] == 0 &&
              memory.read32(request + interface_media_current_offset) ==
                  std::optional<std::uint32_t>{active_media} &&
              memory.read32(request + interface_media_status_offset) ==
                  std::optional<std::uint32_t>{media_status_valid |
                                               media_status_active} &&
              memory.read32(request + interface_media_count_offset) ==
                  std::optional<std::uint32_t>{2},
          "SIOCGIFMEDIA did not expose active virtual en0 media");

  require(memory.write32(request + interface_media_count_offset, 2) &&
              memory.write32(request + interface_media_list_offset, media_list),
          "SIOCGIFMEDIA list request setup failed");
  issue_ioctl(ipv4_fd, get_media_command);
  require(cpu.registers()[0] == 0 &&
              memory.read32(media_list) ==
                  std::optional<std::uint32_t>{media_type_ethernet |
                                               media_subtype_auto} &&
              memory.read32(media_list + 4) ==
                  std::optional<std::uint32_t>{active_media},
          "SIOCGIFMEDIA did not copy out available media words");

  constexpr std::uint32_t arm32_ifreq_size = 32;
  const auto get_mtu_command =
      ioctl_get_interface_mtu | (arm32_ifreq_size << 16U);
  write_interface_name();
  issue_ioctl(ipv4_fd, get_mtu_command);
  require(cpu.registers()[0] == 0 &&
              memory.read32(request + interface_request_value_offset) ==
                  std::optional<std::uint32_t>{default_ethernet_mtu},
          "SIOCGIFMTU did not return virtual en0 MTU");
  constexpr std::uint32_t configured_mtu = 1'400;
  require(
      memory.write32(request + interface_request_value_offset, configured_mtu),
      "SIOCSIFMTU input write failed");
  const auto set_mtu_command =
      ioctl_set_interface_mtu | (arm32_ifreq_size << 16U);
  issue_ioctl(ipv4_fd, set_mtu_command);
  require(cpu.registers()[0] == 0, "SIOCSIFMTU rejected a valid virtual MTU");
  issue_ioctl(ipv4_fd, get_mtu_command);
  require(cpu.registers()[0] == 0 &&
              memory.read32(request + interface_request_value_offset) ==
                  std::optional<std::uint32_t>{configured_mtu},
          "SIOCSIFMTU did not persist the virtual MTU");

  require(
      memory.write32(request + interface_request_value_offset, active_media),
      "SIOCSIFMEDIA input write failed");
  issue_ioctl(ipv4_fd, ioctl_set_interface_media | (arm32_ifreq_size << 16U));
  require(cpu.registers()[0] == 0,
          "SIOCSIFMEDIA rejected virtual Ethernet media");

  write_interface_name();
  constexpr std::uint32_t representative_in6_ifreq_size = 256;
  issue_ioctl(ipv6_fd, ioctl_get_ipv6_address_flags |
                           (representative_in6_ifreq_size << 16U));
  require(cpu.registers()[0] == 0 &&
              memory.read32(request + interface_request_value_offset) ==
                  std::optional<std::uint32_t>{0},
          "SIOCGIFAFLAG_IN6 did not return stable address flags");
}

} // namespace

void run_wifi_tests() {
  wifi_state_test();
  configd_network_ioctl_test();
}

} // namespace ilemu::test::network_suite
