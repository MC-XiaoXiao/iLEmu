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
#include "ilemu/apple80211_hle.hpp"
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

void darwin_network_abi_test() {
  using namespace darwin::network;
  require(ip_type_of_service == 3 && ip_time_to_live == 4 &&
              ip_multicast_interface == 9 && ip_multicast_ttl == 10 &&
              ip_multicast_loop == 11 && ip_add_membership == 12 &&
              ip_drop_membership == 13 && ipv6_unicast_hops == 4 &&
              ipv6_multicast_interface == 9 && ipv6_multicast_hops == 10 &&
              ipv6_multicast_loop == 11 && ipv6_join_group == 12 &&
              ipv6_leave_group == 13 && ipv6_packet_info == 19 &&
              ipv6_hop_limit == 20 && ipv6_only == 27 &&
              ipv4_membership_size == 8 && ipv6_membership_size == 20,
          "Darwin 8 multicast socket constants changed");
  InterfaceSnapshot loopback;
  loopback.name = "lo0";
  loopback.index = 1;
  loopback.flags =
      interface_flag_loopback | interface_flag_running | interface_flag_up;
  loopback.family = interface_family_loopback;
  loopback.mtu = default_loopback_mtu;
  loopback.type = interface_type_loopback;
  std::array<std::byte, 16> loopback_address{};
  loopback_address[0] = std::byte{16};
  loopback_address[1] = std::byte{address_family_inet};
  loopback_address[4] = std::byte{127};
  loopback_address[7] = std::byte{1};
  std::array<std::byte, 16> loopback_mask{};
  loopback_mask[0] = std::byte{16};
  loopback_mask[1] = std::byte{address_family_inet};
  loopback_mask[4] = std::byte{255};
  loopback.ipv4_address = loopback_address;
  loopback.ipv4_netmask = loopback_mask;

  InterfaceSnapshot ethernet;
  ethernet.name = "en0";
  ethernet.index = 2;
  ethernet.flags = interface_flag_broadcast | interface_flag_running |
                   interface_flag_simplex | interface_flag_multicast;
  ethernet.family = interface_family_ethernet;
  ethernet.mtu = default_ethernet_mtu;
  ethernet.type = interface_type_ethernet;
  ethernet.link_address = {std::byte{2}, std::byte{0}, std::byte{0},
                           std::byte{0}, std::byte{0}, std::byte{1}};
  ethernet.link_address_length = 6;

  const std::array interfaces{loopback, ethernet};
  const auto records = make_route_interface_list(interfaces);
  const auto get16 = [&](std::size_t offset) {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(records[offset]) |
        (std::to_integer<std::uint8_t>(records[offset + 1]) << 8U));
  };
  const auto get32 = [&](std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
      value |= static_cast<std::uint32_t>(
                   std::to_integer<std::uint8_t>(records[offset + index]))
               << (index * 8U);
    }
    return value;
  };

  const auto loopback_info_size = get16(0);
  require(loopback_info_size == 132 &&
              std::to_integer<std::uint8_t>(records[2]) ==
                  route_message_version &&
              std::to_integer<std::uint8_t>(records[3]) ==
                  route_message_interface_info &&
              get32(4) == route_address_interface && get16(12) == 1,
          "Darwin if_msghdr layout does not match XNU 792");
  require(std::to_integer<std::uint8_t>(records[112 + 1]) ==
                  address_family_link &&
              records[120] == std::byte{'l'} &&
              records[121] == std::byte{'o'} && records[122] == std::byte{'0'},
          "Darwin sockaddr_dl did not encode the interface name");

  const auto address_offset = static_cast<std::size_t>(loopback_info_size);
  const auto address_size = get16(address_offset);
  require(address_size == 68 &&
              std::to_integer<std::uint8_t>(records[address_offset + 3]) ==
                  route_message_new_address &&
              get32(address_offset + 4) ==
                  (route_address_netmask | route_address_interface_address |
                   route_address_broadcast) &&
              std::equal(records.begin() +
                             static_cast<std::ptrdiff_t>(address_offset + 52),
                         records.begin() +
                             static_cast<std::ptrdiff_t>(address_offset + 68),
                         loopback_address.begin()),
          "Darwin loopback ifa_dstaddr/RTAX_BRD record is malformed");
  const auto ethernet_offset = address_offset + address_size;
  require(get16(ethernet_offset) == 132 && get16(ethernet_offset + 12) == 2,
          "Darwin Ethernet interface record is malformed");

  const auto event_data = make_network_event_data(loopback);
  const auto event = make_kernel_event(
      7, kernel_event_vendor_apple, kernel_event_network_class,
      kernel_event_data_link_subclass, kernel_event_interface_flags_changed,
      event_data);
  require(event.size() == 48 && std::to_integer<std::uint8_t>(event[0]) == 48 &&
              std::to_integer<std::uint8_t>(event[4]) ==
                  kernel_event_vendor_apple &&
              std::to_integer<std::uint8_t>(event[16]) == 7 &&
              event[32] == std::byte{'l'} &&
              event[33] == std::byte{'o'} && event[34] == std::byte{},
          "Darwin kern_event_msg layout is malformed");
}

void darwin_route_socket_test() {
  using namespace darwin;
  const auto put16 = [](std::vector<std::byte> &bytes, std::size_t offset,
                        std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
  };
  const auto put32 = [](std::vector<std::byte> &bytes, std::size_t offset,
                        std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
      bytes[offset + index] =
          static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
  };
  const auto make_message = [&](std::uint8_t family, std::uint8_t command,
                                std::uint8_t gateway_tail,
                                std::string_view interface_name,
                                std::uint8_t version = route::message_version) {
    const std::size_t address_size =
        family == network::address_family_inet ? 16U : 28U;
    const std::size_t link_size = interface_name.empty() ? 0U : 20U;
    std::vector<std::byte> bytes(route::message_header_size +
                                 address_size * 3U + link_size);
    put16(bytes, 0, static_cast<std::uint16_t>(bytes.size()));
    bytes[2] = static_cast<std::byte>(version);
    bytes[3] = static_cast<std::byte>(command);
    put32(bytes, route::header_flags_offset,
          route::flag_up | route::flag_gateway | route::flag_static);
    put32(bytes, route::header_addresses_offset,
          route::address_destination | route::address_gateway |
              route::address_netmask |
              (interface_name.empty() ? 0U : route::address_interface));
    put32(bytes, route::header_sequence_offset, 7);

    auto offset = route::message_header_size;
    for (std::size_t address_index = 0; address_index < 3; ++address_index) {
      bytes[offset] = static_cast<std::byte>(address_size);
      bytes[offset + 1] = static_cast<std::byte>(family);
      if (address_index == 1) {
        if (family == network::address_family_inet) {
          bytes[offset + 4] = std::byte{10};
          bytes[offset + 6] = std::byte{2};
          bytes[offset + 7] = static_cast<std::byte>(gateway_tail);
        } else {
          bytes[offset + 8] = std::byte{0xfe};
          bytes[offset + 9] = std::byte{0x80};
          bytes[offset + 23] = static_cast<std::byte>(gateway_tail);
        }
      }
      offset += address_size;
    }
    if (!interface_name.empty()) {
      bytes[offset] = std::byte{20};
      bytes[offset + 1] = static_cast<std::byte>(network::address_family_link);
      put16(bytes, offset + 2, 2);
      bytes[offset + 5] = static_cast<std::byte>(interface_name.size());
      std::copy(interface_name.begin(), interface_name.end(),
                reinterpret_cast<char *>(bytes.data() + offset + 8));
    }
    return bytes;
  };
  const auto make_query = [&](std::uint8_t command, std::uint32_t sequence,
                              std::array<std::uint8_t, 4> target = {8, 8, 8,
                                                                    8}) {
    constexpr std::size_t ipv4_sockaddr_size = 16;
    std::vector<std::byte> bytes(route::message_header_size +
                                 ipv4_sockaddr_size);
    put16(bytes, 0, static_cast<std::uint16_t>(bytes.size()));
    bytes[2] = static_cast<std::byte>(route::message_version);
    bytes[3] = static_cast<std::byte>(command);
    put32(bytes, route::header_addresses_offset, route::address_destination);
    put32(bytes, route::header_sequence_offset, sequence);
    const auto destination = route::message_header_size;
    bytes[destination] = std::byte{ipv4_sockaddr_size};
    bytes[destination + 1] =
        static_cast<std::byte>(network::address_family_inet);
    for (std::size_t index = 0; index < target.size(); ++index) {
      bytes[destination + 4 + index] = static_cast<std::byte>(target[index]);
    }
    return bytes;
  };

  const auto ipv6 =
      make_message(network::address_family_inet6, route::message_add, 1, "en0");
  const auto parsed_ipv6 = route::parse_message(ipv6);
  const auto ipv6_entry = parsed_ipv6.message
                              ? route::make_entry(*parsed_ipv6.message)
                              : std::nullopt;
  require(parsed_ipv6.message && ipv6_entry &&
              ipv6_entry->family == network::address_family_inet6 &&
              ipv6_entry->interface_name == "en0" &&
              ipv6_entry->gateway.size() == 28 &&
              ipv6_entry->gateway[23] == std::byte{1},
          "Darwin IPv6 routing message did not parse using ARM32 layout");
  route::Table ipv6_table;
  require(ipv6_table.apply(route::message_add, *ipv6_entry) ==
              route::ApplyResult::Applied,
          "IPv6 default route did not enter the route table");
  auto ipv6_query_entry = *ipv6_entry;
  ipv6_query_entry.gateway.clear();
  ipv6_query_entry.netmask.clear();
  ipv6_query_entry.destination[8] = std::byte{0x20};
  ipv6_query_entry.destination[9] = std::byte{0x01};
  const auto ipv6_match = ipv6_table.lookup(ipv6_query_entry);
  const auto ipv6_dump = route::make_table_dump(ipv6_table.snapshot(),
                                                network::address_family_inet6);
  require(ipv6_match && ipv6_match->gateway[23] == std::byte{1} &&
              ipv6_dump.size() == 176 &&
              std::to_integer<std::uint8_t>(ipv6_dump[3]) == route::message_get,
          "IPv6 default lookup or NET_RT_DUMP encoding failed");

  AddressSpace memory;
  constexpr std::uint32_t request_address = 0x3e000;
  constexpr std::uint32_t response_address = request_address + 0x400;
  require(memory.map(request_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "route socket test page failed to map");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  const auto create_route_socket = [&](std::uint32_t protocol =
                                           network::address_family_inet) {
    cpu.registers()[0] = route::protocol_family;
    cpu.registers()[1] = socket::raw;
    cpu.registers()[2] = protocol;
    cpu.registers()[12] = syscall::socket;
    kernel.dispatch(cpu, 0x80);
    require((cpu.cpsr() & (1U << 29U)) == 0, "PF_ROUTE socket creation failed");
    return cpu.registers()[0];
  };
  const auto write_route = [&](std::uint32_t fd,
                               const std::vector<std::byte> &message) {
    require(memory.copy_in(request_address, message),
            "route message copy-in failed");
    cpu.registers()[0] = fd;
    cpu.registers()[1] = request_address;
    cpu.registers()[2] = static_cast<std::uint32_t>(message.size());
    cpu.registers()[12] = syscall::write;
    kernel.dispatch(cpu, 0x80);
  };

  const auto writer = create_route_socket();
  const auto listener = create_route_socket();
  const auto ipv6_listener = create_route_socket(network::address_family_inet6);
  constexpr std::uint32_t descriptor_set = request_address + 0xc00;
  constexpr std::uint32_t zero_timeout = request_address + 0xc10;
  const auto poll_readable = [&](std::uint32_t fd) {
    require(fd < 32 && memory.write32(descriptor_set, 1U << fd) &&
                memory.write32(zero_timeout, 0) &&
                memory.write32(zero_timeout + 4, 0),
            "route select poll setup failed");
    cpu.registers()[0] = fd + 1;
    cpu.registers()[1] = descriptor_set;
    cpu.registers()[2] = 0;
    cpu.registers()[3] = 0;
    cpu.registers()[4] = zero_timeout;
    cpu.registers()[12] = 93;
    kernel.dispatch(cpu, 0x80);
    require((cpu.cpsr() & (1U << 29U)) == 0, "route select poll failed");
    return cpu.registers()[0];
  };
  const auto added =
      make_message(network::address_family_inet, route::message_add, 1, "en0");
  write_route(writer, added);
  require((cpu.cpsr() & (1U << 29U)) == 0 && cpu.registers()[0] == added.size(),
          "RTM_ADD did not report the written message size");
  auto routes = kernel.route_snapshot();
  require(routes.size() == 1 && routes.front().interface_name == "en0" &&
              routes.front().interface_index == 2 &&
              routes.front().gateway.size() == 16 &&
              routes.front().gateway[7] == std::byte{1},
          "RTM_ADD did not populate the shared virtual route table");

  cpu.registers()[0] = listener;
  cpu.registers()[1] = response_address;
  cpu.registers()[2] = static_cast<std::uint32_t>(added.size());
  cpu.registers()[12] = syscall::read;
  kernel.dispatch(cpu, 0x80);
  require((cpu.cpsr() & (1U << 29U)) == 0 &&
              cpu.registers()[0] == added.size() &&
              memory.read32(response_address + route::header_pid_offset) ==
                  std::optional<std::uint32_t>{kernel.process().pid} &&
              (memory.read32(response_address + route::header_flags_offset)
                   .value_or(0) &
               route::flag_done) != 0,
          "PF_ROUTE did not broadcast the XNU completion response");
  require(poll_readable(ipv6_listener) == 0,
          "AF_INET route message leaked to an AF_INET6 route socket");

  cpu.registers()[0] = listener;
  cpu.registers()[12] = 41; // dup
  kernel.dispatch(cpu, 0x80);
  require((cpu.cpsr() & (1U << 29U)) == 0, "route socket dup failed");
  const auto duplicated_listener = cpu.registers()[0];

  const auto query_socket = create_route_socket();
  auto silent_query = make_query(route::message_get_silent, 100);
  constexpr std::size_t empty_link_offset = route::message_header_size + 16;
  silent_query.resize(silent_query.size() + 20);
  put16(silent_query, 0, static_cast<std::uint16_t>(silent_query.size()));
  put32(silent_query, route::header_addresses_offset,
        route::address_destination | route::address_interface);
  silent_query[empty_link_offset] = std::byte{20};
  silent_query[empty_link_offset + 1] =
      static_cast<std::byte>(network::address_family_link);
  write_route(query_socket, silent_query);
  require((cpu.cpsr() & (1U << 29U)) == 0,
          "RTM_GET_SILENT default-route lookup failed");
  cpu.registers()[0] = query_socket;
  cpu.registers()[1] = response_address;
  cpu.registers()[2] = 256;
  cpu.registers()[12] = syscall::read;
  kernel.dispatch(cpu, 0x80);
  require((cpu.cpsr() & (1U << 29U)) == 0 && cpu.registers()[0] == 160 &&
              memory.read8(response_address + 3) ==
                  std::optional<std::uint8_t>{route::message_get} &&
              memory.read32(response_address + route::header_sequence_offset) ==
                  std::optional<std::uint32_t>{100} &&
              memory.read8(response_address + 115) ==
                  std::optional<std::uint8_t>{1} &&
              memory.read8(response_address + 145) ==
                  std::optional<std::uint8_t>{3} &&
              memory.read_c_string(response_address + 148, 4) ==
                  std::optional<std::string>{"en0"},
          "Reachability RTM_GET_SILENT did not return gateway/interface");

  const auto broadcast_query = make_query(route::message_get, 101);
  write_route(query_socket, broadcast_query);
  cpu.registers()[0] = listener;
  cpu.registers()[1] = response_address;
  cpu.registers()[2] = 256;
  cpu.registers()[12] = syscall::read;
  kernel.dispatch(cpu, 0x80);
  require((cpu.cpsr() & (1U << 29U)) == 0 &&
              memory.read32(response_address + route::header_sequence_offset) ==
                  std::optional<std::uint32_t>{101},
          "RTM_GET_SILENT leaked to another route listener");
  require(poll_readable(duplicated_listener) == 0,
          "duplicated route descriptor received an already-consumed message");

  auto subnet =
      make_message(network::address_family_inet, route::message_add, 9, "en0");
  constexpr std::size_t destination_address_offset =
      route::message_header_size + 4;
  constexpr std::size_t netmask_address_offset =
      route::message_header_size + 16U * 2U + 4U;
  subnet[destination_address_offset] = std::byte{10};
  subnet[destination_address_offset + 2] = std::byte{2};
  subnet[netmask_address_offset] = std::byte{255};
  subnet[netmask_address_offset + 1] = std::byte{255};
  subnet[netmask_address_offset + 2] = std::byte{255};
  write_route(writer, subnet);
  require((cpu.cpsr() & (1U << 29U)) == 0 &&
              kernel.route_snapshot().size() == 2,
          "subnet RTM_ADD failed before longest-prefix lookup");
  const auto prefix_query_socket = create_route_socket();
  const auto prefix_query = make_query(route::message_get, 102, {10, 0, 2, 99});
  write_route(prefix_query_socket, prefix_query);
  cpu.registers()[0] = prefix_query_socket;
  cpu.registers()[1] = response_address;
  cpu.registers()[2] = 256;
  cpu.registers()[12] = syscall::read;
  kernel.dispatch(cpu, 0x80);
  require((cpu.cpsr() & (1U << 29U)) == 0 &&
              memory.read8(response_address + 115) ==
                  std::optional<std::uint8_t>{9},
          "RTM_GET did not prefer the matching /24 over the default route");
  auto subnet_delete = subnet;
  subnet_delete[3] = static_cast<std::byte>(route::message_delete);
  write_route(writer, subnet_delete);
  require((cpu.cpsr() & (1U << 29U)) == 0 &&
              kernel.route_snapshot().size() == 1,
          "subnet cleanup route delete failed");

  write_route(writer, added);
  require((cpu.cpsr() & (1U << 29U)) != 0 &&
              cpu.registers()[0] == error::file_exists &&
              kernel.route_snapshot().size() == 1,
          "duplicate RTM_ADD did not return EEXIST");

  const auto changed = make_message(network::address_family_inet,
                                    route::message_change, 3, "en0");
  write_route(writer, changed);
  routes = kernel.route_snapshot();
  require((cpu.cpsr() & (1U << 29U)) == 0 && routes.size() == 1 &&
              routes.front().gateway[7] == std::byte{3},
          "RTM_CHANGE did not replace the gateway");

  constexpr std::uint32_t mib_address = request_address + 0x800;
  constexpr std::uint32_t old_size_address = request_address + 0x820;
  constexpr std::uint32_t dump_address = request_address + 0x900;
  const std::array<std::uint32_t, 6> dump_mib{
      4,
      route::protocol_family,
      0,
      network::address_family_unspecified,
      route::sysctl_dump,
      0};
  for (std::size_t index = 0; index < dump_mib.size(); ++index) {
    require(memory.write32(mib_address + static_cast<std::uint32_t>(index * 4U),
                           dump_mib[index]),
            "NET_RT_DUMP MIB write failed");
  }
  require(memory.write32(old_size_address, 0), "NET_RT_DUMP size setup failed");
  cpu.registers()[0] = mib_address;
  cpu.registers()[1] = static_cast<std::uint32_t>(dump_mib.size());
  cpu.registers()[2] = 0;
  cpu.registers()[3] = old_size_address;
  cpu.registers()[4] = 0;
  cpu.registers()[5] = 0;
  cpu.registers()[12] = 202;
  kernel.dispatch(cpu, 0x80);
  const auto dump_size = memory.read32(old_size_address).value_or(0);
  require((cpu.cpsr() & (1U << 29U)) == 0 && dump_size == 140,
          "NET_RT_DUMP size query did not expose one route record");
  require(memory.write32(old_size_address, dump_size),
          "NET_RT_DUMP capacity setup failed");
  cpu.registers()[0] = mib_address;
  cpu.registers()[1] = static_cast<std::uint32_t>(dump_mib.size());
  cpu.registers()[2] = dump_address;
  cpu.registers()[3] = old_size_address;
  cpu.registers()[4] = 0;
  cpu.registers()[5] = 0;
  cpu.registers()[12] = 202;
  kernel.dispatch(cpu, 0x80);
  require(
      (cpu.cpsr() & (1U << 29U)) == 0 &&
          memory.read16(dump_address) == std::optional<std::uint16_t>{140} &&
          memory.read8(dump_address + 3) ==
              std::optional<std::uint8_t>{route::message_get} &&
          (memory.read32(dump_address + route::header_flags_offset)
               .value_or(0) &
           route::flag_done) == 0 &&
          memory.read32(dump_address + route::header_pid_offset) ==
              std::optional<std::uint32_t>{0} &&
          memory.read8(dump_address + 115) == std::optional<std::uint8_t>{3},
      "NET_RT_DUMP record did not encode the changed virtual route");
  const auto filtered_dump_size = [&](std::uint32_t flags) {
    require(memory.write32(mib_address + 16, route::sysctl_flags) &&
                memory.write32(mib_address + 20, flags) &&
                memory.write32(old_size_address, 0),
            "NET_RT_FLAGS MIB setup failed");
    cpu.registers()[0] = mib_address;
    cpu.registers()[1] = static_cast<std::uint32_t>(dump_mib.size());
    cpu.registers()[2] = 0;
    cpu.registers()[3] = old_size_address;
    cpu.registers()[4] = 0;
    cpu.registers()[5] = 0;
    cpu.registers()[12] = 202;
    kernel.dispatch(cpu, 0x80);
    require((cpu.cpsr() & (1U << 29U)) == 0, "NET_RT_FLAGS size query failed");
    return memory.read32(old_size_address).value_or(0);
  };
  require(filtered_dump_size(route::flag_gateway) == 140 &&
              filtered_dump_size(route::flag_host) == 0,
          "NET_RT_FLAGS did not apply XNU any-flag filtering");
  require(memory.write32(mib_address + 16, route::sysctl_dump2) &&
              memory.write32(mib_address + 20, 0) &&
              memory.write32(old_size_address, 0),
          "NET_RT_DUMP2 MIB setup failed");
  cpu.registers()[0] = mib_address;
  cpu.registers()[1] = static_cast<std::uint32_t>(dump_mib.size());
  cpu.registers()[2] = 0;
  cpu.registers()[3] = old_size_address;
  cpu.registers()[4] = 0;
  cpu.registers()[5] = 0;
  cpu.registers()[12] = 202;
  kernel.dispatch(cpu, 0x80);
  require((cpu.cpsr() & (1U << 29U)) == 0 &&
              memory.read32(old_size_address) ==
                  std::optional<std::uint32_t>{140},
          "NET_RT_DUMP2 size query failed");
  require(memory.write32(old_size_address, 140),
          "NET_RT_DUMP2 capacity setup failed");
  cpu.registers()[0] = mib_address;
  cpu.registers()[1] = static_cast<std::uint32_t>(dump_mib.size());
  cpu.registers()[2] = dump_address;
  cpu.registers()[3] = old_size_address;
  cpu.registers()[4] = 0;
  cpu.registers()[5] = 0;
  cpu.registers()[12] = 202;
  kernel.dispatch(cpu, 0x80);
  require(
      (cpu.cpsr() & (1U << 29U)) == 0 &&
          memory.read8(dump_address + 3) ==
              std::optional<std::uint8_t>{route::message_get2} &&
          memory.read32(dump_address + route::header2_reference_count_offset) ==
              std::optional<std::uint32_t>{0} &&
          memory.read32(dump_address + route::header2_parent_flags_offset) ==
              std::optional<std::uint32_t>{0} &&
          memory.read32(dump_address + route::header2_reserved_offset) ==
              std::optional<std::uint32_t>{0},
      "NET_RT_DUMP2 did not encode refcnt/parent/reserved fields");

  const auto invalid_interface = make_message(network::address_family_inet,
                                              route::message_change, 4, "bad0");
  write_route(writer, invalid_interface);
  require((cpu.cpsr() & (1U << 29U)) != 0 &&
              cpu.registers()[0] == error::no_such_device_or_address,
          "route operation accepted an unknown interface");

  const auto removed =
      make_message(network::address_family_inet, route::message_delete, 0, {});
  write_route(writer, removed);
  require((cpu.cpsr() & (1U << 29U)) == 0 && kernel.route_snapshot().empty(),
          "RTM_DELETE did not remove the default route by destination/mask");
  write_route(writer, removed);
  require((cpu.cpsr() & (1U << 29U)) != 0 &&
              cpu.registers()[0] == error::no_such_process,
          "missing RTM_DELETE did not return ESRCH");

  const auto missing_query_socket = create_route_socket();
  const auto missing_query = make_query(route::message_get, 103);
  write_route(missing_query_socket, missing_query);
  require((cpu.cpsr() & (1U << 29U)) != 0 &&
              cpu.registers()[0] == error::no_such_process,
          "missing RTM_GET did not return ESRCH");
  cpu.registers()[0] = missing_query_socket;
  cpu.registers()[1] = response_address;
  cpu.registers()[2] = 256;
  cpu.registers()[12] = syscall::read;
  kernel.dispatch(cpu, 0x80);
  require((cpu.cpsr() & (1U << 29U)) == 0 &&
              memory.read32(response_address + route::header_error_offset) ==
                  std::optional<std::uint32_t>{error::no_such_process} &&
              (memory.read32(response_address + route::header_flags_offset)
                   .value_or(0) &
               route::flag_done) == 0,
          "failed RTM_GET response incorrectly reported RTF_DONE");

  kernel.process().uid = 501;
  kernel.process().effective_uid = 501;
  write_route(writer, added);
  require((cpu.cpsr() & (1U << 29U)) != 0 &&
              cpu.registers()[0] == error::operation_not_permitted &&
              kernel.route_snapshot().empty(),
          "unprivileged guest process modified the route table");
  kernel.process().uid = 0;
  kernel.process().effective_uid = 0;

  const auto malformed = make_message(network::address_family_inet,
                                      route::message_add, 1, "en0", 4);
  write_route(writer, malformed);
  require((cpu.cpsr() & (1U << 29U)) != 0 &&
              cpu.registers()[0] == error::protocol_not_supported,
          "PF_ROUTE accepted an unsupported routing message version");
}

void interface_route_synchronization_test() {
  using namespace darwin;
  AddressSpace memory;
  constexpr std::uint32_t request_address = 0x45000;
  constexpr std::uint32_t response_address = request_address + 0x800;
  require(memory.map(request_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "interface-route test page failed to map");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  const auto create_socket = [&](std::uint32_t family, std::uint32_t type,
                                 std::uint32_t protocol) {
    cpu.registers()[0] = family;
    cpu.registers()[1] = type;
    cpu.registers()[2] = protocol;
    cpu.registers()[12] = syscall::socket;
    kernel.dispatch(cpu, 0x80);
    require((cpu.cpsr() & (1U << 29U)) == 0,
            "interface-route socket creation failed");
    return cpu.registers()[0];
  };
  const auto ipv4_control =
      create_socket(network::address_family_inet, socket::datagram, 0);
  const auto route_listener = create_socket(
      route::protocol_family, socket::raw, network::address_family_unspecified);
  const auto issue_ioctl = [&](std::uint32_t fd, std::uint32_t command,
                               const std::vector<std::byte> &request) {
    require(memory.copy_in(request_address, request),
            "interface-route ioctl request copy failed");
    cpu.registers()[0] = fd;
    cpu.registers()[1] = command;
    cpu.registers()[2] = request_address;
    cpu.registers()[12] = 54;
    kernel.dispatch(cpu, 0x80);
  };
  const auto read_route_type = [&] {
    cpu.registers()[0] = route_listener;
    cpu.registers()[1] = response_address;
    cpu.registers()[2] = 256;
    cpu.registers()[12] = syscall::read;
    kernel.dispatch(cpu, 0x80);
    require((cpu.cpsr() & (1U << 29U)) == 0 &&
                memory.read32(response_address + route::header_pid_offset) ==
                    std::optional<std::uint32_t>{0} &&
                (memory.read32(response_address + route::header_flags_offset)
                     .value_or(0) &
                 route::flag_done) == 0,
            "generated interface route event has invalid kernel fields");
    return memory.read8(response_address + 3).value_or(0);
  };
  const auto make_ipv4_request = [](std::array<std::uint8_t, 4> address) {
    std::vector<std::byte> request(120);
    request[0] = std::byte{'e'};
    request[1] = std::byte{'n'};
    request[2] = std::byte{'0'};
    for (const auto offset : {16U, 32U, 48U}) {
      request[offset] = std::byte{16};
      request[offset + 1] =
          static_cast<std::byte>(network::address_family_inet);
    }
    for (std::size_t index = 0; index < address.size(); ++index) {
      request[20 + index] = static_cast<std::byte>(address[index]);
    }
    request[36] = static_cast<std::byte>(address[0]);
    request[37] = static_cast<std::byte>(address[1]);
    request[38] = static_cast<std::byte>(address[2]);
    request[39] = std::byte{255};
    request[52] = std::byte{255};
    request[53] = std::byte{255};
    request[54] = std::byte{255};
    return request;
  };

  auto ipv4_request = make_ipv4_request({192, 0, 2, 7});
  issue_ioctl(ipv4_control, 0x8040691aU, ipv4_request);
  auto routes = kernel.route_snapshot();
  require((cpu.cpsr() & (1U << 29U)) == 0 && routes.size() == 1 &&
              routes.front().origin == route::Entry::Origin::Interface &&
              routes.front().destination[4] == std::byte{192} &&
              routes.front().destination[6] == std::byte{2} &&
              routes.front().destination[7] == std::byte{0} &&
              read_route_type() == route::message_add,
          "SIOCAIFADDR did not create and announce the connected route");

  ipv4_request = make_ipv4_request({198, 51, 100, 7});
  issue_ioctl(ipv4_control, 0x8040691aU, ipv4_request);
  routes = kernel.route_snapshot();
  require((cpu.cpsr() & (1U << 29U)) == 0 && routes.size() == 1 &&
              routes.front().destination[4] == std::byte{198} &&
              routes.front().destination[5] == std::byte{51} &&
              routes.front().destination[6] == std::byte{100} &&
              read_route_type() == route::message_delete &&
              read_route_type() == route::message_add,
          "IPv4 address replacement did not replace its connected route");

  issue_ioctl(ipv4_control, 0x80206919U, ipv4_request);
  require((cpu.cpsr() & (1U << 29U)) == 0 && kernel.route_snapshot().empty() &&
              read_route_type() == route::message_delete,
          "SIOCDIFADDR did not delete and announce the connected route");
  issue_ioctl(ipv4_control, 0x80206919U, ipv4_request);
  require((cpu.cpsr() & (1U << 29U)) != 0 &&
              cpu.registers()[0] == error::address_not_available,
          "SIOCDIFADDR did not reject an unconfigured address");

  const auto ipv6_control =
      create_socket(network::address_family_inet6, socket::datagram, 0);
  std::vector<std::byte> ipv6_request(120);
  ipv6_request[0] = std::byte{'e'};
  ipv6_request[1] = std::byte{'n'};
  ipv6_request[2] = std::byte{'0'};
  ipv6_request[16] = std::byte{28};
  ipv6_request[17] = static_cast<std::byte>(network::address_family_inet6);
  ipv6_request[24] = std::byte{0x20};
  ipv6_request[25] = std::byte{0x01};
  ipv6_request[26] = std::byte{0x0d};
  ipv6_request[27] = std::byte{0xb8};
  ipv6_request[29] = std::byte{1};
  ipv6_request[39] = std::byte{7};
  ipv6_request[72] = std::byte{28};
  ipv6_request[73] = static_cast<std::byte>(network::address_family_inet6);
  std::fill(ipv6_request.begin() + 80, ipv6_request.begin() + 88,
            std::byte{0xff});
  issue_ioctl(ipv6_control, 0x8078691aU, ipv6_request);
  routes = kernel.route_snapshot();
  const auto host_routes =
      std::count_if(routes.begin(), routes.end(), [](const auto &entry) {
        return (entry.flags & route::flag_host) != 0;
      });
  const auto prefix_routes =
      std::count_if(routes.begin(), routes.end(), [](const auto &entry) {
        return (entry.flags & route::flag_cloning) != 0;
      });
  require((cpu.cpsr() & (1U << 29U)) == 0 && routes.size() == 2 &&
              host_routes == 1 && prefix_routes == 1 &&
              read_route_type() == route::message_add &&
              read_route_type() == route::message_add,
          "IPv6 address did not create prefix and local-host routes");
  issue_ioctl(ipv6_control, 0x81006919U, ipv6_request);
  require((cpu.cpsr() & (1U << 29U)) == 0 && kernel.route_snapshot().empty() &&
              read_route_type() == route::message_delete &&
              read_route_type() == route::message_delete,
          "SIOCDIFADDR_IN6 did not remove generated IPv6 routes");
}

void network_route_and_kernel_event_test() {
  using namespace darwin::network;
  AddressSpace memory;
  constexpr std::uint32_t base = 0x3e000;
  constexpr std::uint32_t filter_address = base;
  constexpr std::uint32_t interface_request = base + 0x40;
  constexpr std::uint32_t event_output = base + 0x100;
  constexpr std::uint32_t select_set = base + 0x180;
  constexpr std::uint32_t mib_address = base + 0x200;
  constexpr std::uint32_t old_size_address = base + 0x220;
  constexpr std::uint32_t kevent_change = base + 0x280;
  constexpr std::uint32_t kevent_output = base + 0x2c0;
  constexpr std::uint32_t route_output = base + 0x400;
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "network ABI memory map failed");

  Dynarmic::ExclusiveMonitor monitor{3};
  Cpu reader{0, memory, monitor};
  Cpu selector{1, memory, monitor};
  Cpu controller{2, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  const auto create_event_socket = [&] {
    controller.registers()[0] = 32; // PF_SYSTEM
    controller.registers()[1] = 3;  // SOCK_RAW
    controller.registers()[2] = 1;  // SYSPROTO_EVENT
    controller.registers()[12] = 97;
    kernel.dispatch(controller, 0x80);
    return controller.registers()[0];
  };
  const auto read_event_fd = create_event_socket();
  const auto select_event_fd = create_event_socket();
  require(read_event_fd >= 3 && select_event_fd > read_event_fd,
          "PF_SYSTEM event socket creation failed");

  require(
      memory.write32(filter_address, kernel_event_vendor_apple) &&
          memory.write32(filter_address + 4, kernel_event_network_class) &&
          memory.write32(filter_address + 8, kernel_event_data_link_subclass),
      "kernel-event filter write failed");
  for (const auto fd : {read_event_fd, select_event_fd}) {
    controller.registers()[0] = fd;
    controller.registers()[1] = 0x800c6502U; // SIOCSKEVFILT
    controller.registers()[2] = filter_address;
    controller.registers()[12] = 54;
    kernel.dispatch(controller, 0x80);
    require(controller.registers()[0] == 0,
            "PF_SYSTEM event filter installation failed");
  }

  reader.registers()[0] = read_event_fd;
  reader.registers()[1] = event_output;
  reader.registers()[2] = 64;
  reader.registers()[12] = darwin::syscall::read;
  kernel.dispatch(reader, 0x80);
  require(kernel.wait_reason(0) ==
              "read(fd=" + std::to_string(read_event_fd) + ")",
          "PF_SYSTEM read did not block before an event");

  require(select_event_fd < 32 &&
              memory.write32(select_set, 1U << select_event_fd),
          "PF_SYSTEM select set write failed");
  selector.registers()[0] = select_event_fd + 1U;
  selector.registers()[1] = select_set;
  selector.registers()[2] = 0;
  selector.registers()[3] = 0;
  selector.registers()[4] = 0;
  selector.registers()[12] = 93;
  kernel.dispatch(selector, 0x80);
  require(kernel.wait_reason(1).starts_with("select("),
          "PF_SYSTEM select did not block before an event");

  std::array<std::byte, 120> request{};
  request[0] = std::byte{'l'};
  request[1] = std::byte{'o'};
  request[2] = std::byte{'0'};
  require(memory.copy_in(interface_request, request),
          "interface request copy failed");

  controller.registers()[0] = 2; // AF_INET
  controller.registers()[1] = 2; // SOCK_DGRAM
  controller.registers()[2] = 0;
  controller.registers()[12] = 97;
  kernel.dispatch(controller, 0x80);
  const auto control_fd = controller.registers()[0];
  require(control_fd > select_event_fd,
          "AF_INET control socket creation failed");

  controller.registers()[0] = control_fd;
  controller.registers()[1] = 0xc0206911U; // SIOCGIFFLAGS
  controller.registers()[2] = interface_request;
  controller.registers()[12] = 54;
  kernel.dispatch(controller, 0x80);
  const auto flags = memory.read16(interface_request + 16);
  require(controller.registers()[0] == 0 && flags,
          "SIOCGIFFLAGS failed for lo0");
  require(memory.write16(interface_request + 16, *flags | interface_flag_up),
          "SIOCSIFFLAGS input write failed");
  controller.registers()[0] = control_fd;
  controller.registers()[1] = 0x80206910U; // SIOCSIFFLAGS
  controller.registers()[2] = interface_request;
  controller.registers()[12] = 54;
  kernel.dispatch(controller, 0x80);
  require(controller.registers()[0] == 0, "SIOCSIFFLAGS failed for lo0");

  require(kernel.deliver_pending_io(reader),
          "kernel event did not wake a blocked PF_SYSTEM read");
  require(
      reader.registers()[0] == 48 &&
          memory.read32(event_output) == std::optional<std::uint32_t>{48} &&
          memory.read32(event_output + 4) ==
              std::optional<std::uint32_t>{kernel_event_vendor_apple} &&
          memory.read32(event_output + 8) ==
              std::optional<std::uint32_t>{kernel_event_network_class} &&
          memory.read32(event_output + 12) ==
              std::optional<std::uint32_t>{kernel_event_data_link_subclass} &&
          memory.read32(event_output + 20) ==
              std::optional<std::uint32_t>{
                  kernel_event_interface_flags_changed} &&
          memory.read32(event_output + 28) ==
              std::optional<std::uint32_t>{0} &&
          memory.read_c_string(event_output + 32, interface_name_size) ==
              std::optional<std::string>{"lo"},
      "PF_SYSTEM returned a malformed XNU kernel event");
  require(kernel.deliver_pending_io(selector) && selector.registers()[0] == 1 &&
              memory.read32(select_set) ==
                  std::optional<std::uint32_t>{1U << select_event_fd},
          "kernel event did not wake PF_SYSTEM select");

  controller.registers()[0] = read_event_fd;
  controller.registers()[1] = 0x40046501U; // SIOCGKEVID
  controller.registers()[2] = filter_address;
  controller.registers()[12] = 54;
  kernel.dispatch(controller, 0x80);
  require(controller.registers()[0] == 0 &&
              memory.read32(filter_address) == std::optional<std::uint32_t>{1},
          "SIOCGKEVID did not expose the latest event identifier");

  // select reports level-triggered readability without consuming the
  // datagram. Drain that socket, then prove EVFILT_READ wakes through kqueue
  // for the next XNU data-link event.
  controller.registers()[0] = select_event_fd;
  controller.registers()[1] = event_output + 64;
  controller.registers()[2] = 64;
  controller.registers()[12] = darwin::syscall::read;
  kernel.dispatch(controller, 0x80);
  require(controller.registers()[0] == 48,
          "PF_SYSTEM event drain after select failed");

  controller.registers()[12] = 362; // kqueue
  kernel.dispatch(controller, 0x80);
  const auto kqueue_fd = controller.registers()[0];
  require(kqueue_fd > control_fd, "kqueue creation for PF_SYSTEM failed");
  require(memory.write32(kevent_change, select_event_fd) &&
              memory.write16(kevent_change + 4, 0xffffU) && // EVFILT_READ
              memory.write16(kevent_change + 6, 1) &&       // EV_ADD
              memory.write32(kevent_change + 8, 0) &&
              memory.write32(kevent_change + 12, 0) &&
              memory.write32(kevent_change + 16, 0x1234),
          "PF_SYSTEM kevent registration write failed");
  controller.registers()[0] = kqueue_fd;
  controller.registers()[1] = kevent_change;
  controller.registers()[2] = 1;
  controller.registers()[3] = 0;
  controller.registers()[4] = 0;
  controller.registers()[5] = 0;
  controller.registers()[12] = 363;
  kernel.dispatch(controller, 0x80);
  require(controller.registers()[0] == 0,
          "PF_SYSTEM EVFILT_READ registration failed");

  selector.registers()[0] = kqueue_fd;
  selector.registers()[1] = 0;
  selector.registers()[2] = 0;
  selector.registers()[3] = kevent_output;
  selector.registers()[4] = 1;
  selector.registers()[5] = 0;
  selector.registers()[12] = 363;
  kernel.dispatch(selector, 0x80);
  require(kernel.wait_reason(1).starts_with("kevent("),
          "PF_SYSTEM kqueue wait did not block before the next event");

  controller.registers()[0] = control_fd;
  controller.registers()[1] = 0x80206910U; // SIOCSIFFLAGS
  controller.registers()[2] = interface_request;
  controller.registers()[12] = 54;
  kernel.dispatch(controller, 0x80);
  require(controller.registers()[0] == 0 &&
              kernel.deliver_pending_io(selector) &&
              selector.registers()[0] == 1 &&
              memory.read32(kevent_output) ==
                  std::optional<std::uint32_t>{select_event_fd} &&
              memory.read16(kevent_output + 4) ==
                  std::optional<std::uint16_t>{0xffffU} &&
              memory.read32(kevent_output + 16) ==
                  std::optional<std::uint32_t>{0x1234},
          "PF_SYSTEM event did not wake EVFILT_READ");

  request = {};
  request[0] = std::byte{'l'};
  request[1] = std::byte{'o'};
  request[2] = std::byte{'0'};
  request[16] = std::byte{16};
  request[17] = std::byte{address_family_inet};
  request[20] = std::byte{127};
  request[23] = std::byte{1};
  request[48] = std::byte{16};
  request[49] = std::byte{address_family_inet};
  request[52] = std::byte{255};
  require(memory.copy_in(interface_request, request),
          "IPv4 ifaliasreq copy failed");
  controller.registers()[0] = control_fd;
  controller.registers()[1] = 0x8040691aU; // SIOCAIFADDR
  controller.registers()[2] = interface_request;
  controller.registers()[12] = 54;
  kernel.dispatch(controller, 0x80);
  require(controller.registers()[0] == 0, "SIOCAIFADDR failed for lo0");

  const std::array<std::uint32_t, 6> mib{4, 17, 0, address_family_unspecified,
                                         3, 0};
  for (std::size_t index = 0; index < mib.size(); ++index) {
    require(memory.write32(mib_address + static_cast<std::uint32_t>(index * 4U),
                           mib[index]),
            "route sysctl MIB write failed");
  }
  require(memory.write32(old_size_address, 0),
          "route sysctl size initialization failed");
  controller.registers()[0] = mib_address;
  controller.registers()[1] = static_cast<std::uint32_t>(mib.size());
  controller.registers()[2] = 0;
  controller.registers()[3] = old_size_address;
  controller.registers()[4] = 0;
  controller.registers()[5] = 0;
  controller.registers()[12] = 202;
  kernel.dispatch(controller, 0x80);
  const auto route_size = memory.read32(old_size_address).value_or(0);
  require(controller.registers()[0] == 0 && route_size > 0 &&
              route_size < AddressSpace::page_size - 0x400,
          "NET_RT_IFLIST size query failed");

  require(memory.write32(old_size_address, route_size),
          "route sysctl capacity write failed");
  controller.registers()[0] = mib_address;
  controller.registers()[1] = static_cast<std::uint32_t>(mib.size());
  controller.registers()[2] = route_output;
  controller.registers()[3] = old_size_address;
  controller.registers()[4] = 0;
  controller.registers()[5] = 0;
  controller.registers()[12] = 202;
  kernel.dispatch(controller, 0x80);
  require(controller.registers()[0] == 0, "NET_RT_IFLIST record query failed");

  std::uint32_t offset = 0;
  std::uint32_t interface_records = 0;
  std::uint32_t address_records = 0;
  while (offset < route_size) {
    const auto length = memory.read16(route_output + offset).value_or(0);
    const auto type = memory.read8(route_output + offset + 3).value_or(0);
    require(length >= 20 && length <= route_size - offset,
            "NET_RT_IFLIST returned an invalid record length");
    if (type == route_message_interface_info)
      ++interface_records;
    if (type == route_message_new_address)
      ++address_records;
    offset += length;
  }
  require(
      offset == route_size && interface_records == 2 && address_records == 1 &&
          memory.read16(route_output + 12) == std::optional<std::uint16_t>{1} &&
          memory.read_c_string(route_output + 120, 4) ==
              std::optional<std::string>{"lo0"},
      "NET_RT_IFLIST did not expose ordered lo0/en0 records");
}

} // namespace

void run_abi_route_tests() {
  darwin_network_abi_test();
  darwin_route_socket_test();
  interface_route_synchronization_test();
  network_route_and_kernel_event_test();
}

} // namespace ilemu::test::network_suite
