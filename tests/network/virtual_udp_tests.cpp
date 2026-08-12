#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/output.hpp"
#include "ilemu/virtual_udp.hpp"

#include "suite.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <sstream>
#include <vector>

namespace ilemu::test::network_suite {
namespace {

using test::require;

constexpr std::uint16_t mdns_port = 5353;
constexpr std::uint32_t virtual_interface_index = 2;
constexpr std::array<std::byte, 4> mdns_group{std::byte{224}, std::byte{0},
                                              std::byte{0}, std::byte{251}};
constexpr std::size_t aligned_control_size(std::size_t payload_size) {
  return (darwin::network::arm32_control_header_size + payload_size + 3U) & ~3U;
}
constexpr auto expected_ipv4_control_size =
    aligned_control_size(4) +
    aligned_control_size(darwin::network::arm32_sockaddr_dl_size) +
    aligned_control_size(1);

std::array<std::byte, darwin::network::arm32_sockaddr_in_size>
make_ipv4_address(std::span<const std::byte, 4> address) {
  std::array<std::byte, darwin::network::arm32_sockaddr_in_size> result{};
  result[darwin::network::sockaddr_length_offset] =
      static_cast<std::byte>(result.size());
  result[darwin::network::sockaddr_family_offset] =
      static_cast<std::byte>(darwin::network::address_family_inet);
  result[darwin::network::sockaddr_port_offset] =
      static_cast<std::byte>(mdns_port >> 8U);
  result[darwin::network::sockaddr_port_offset + 1U] =
      static_cast<std::byte>(mdns_port);
  std::copy(address.begin(), address.end(),
            result.begin() + darwin::network::sockaddr_ipv4_address_offset);
  return result;
}

void mdns_multicast_data_plane_test() {
  const auto network = std::make_shared<bsd::VirtualUdpNetwork>();
  const auto sender = network->create(darwin::network::address_family_inet);
  const auto receiver = network->create(darwin::network::address_family_inet);
  require(sender && receiver, "virtual IPv4 UDP socket creation failed");

  constexpr std::array<std::byte, 4> wildcard{};
  const auto local = make_ipv4_address(wildcard);
  require(sender->bind(local) == bsd::VirtualUdpStatus::Success &&
              receiver->bind(local) == bsd::VirtualUdpStatus::Success,
          "virtual mDNS wildcard bind failed");

  std::array<std::byte, darwin::network::ipv4_membership_size> membership{};
  std::copy(mdns_group.begin(), mdns_group.end(), membership.begin());
  require(receiver->set_option(darwin::network::protocol_ip,
                               darwin::network::ip_add_membership,
                               membership) == bsd::VirtualUdpStatus::Success,
          "virtual mDNS multicast join failed");

  constexpr std::array payload{std::byte{'D'}, std::byte{'N'}, std::byte{'S'}};
  const auto destination = make_ipv4_address(mdns_group);
  require(
      sender->send(payload, destination) == bsd::VirtualUdpStatus::Success &&
          receiver->readable() && receiver->pending_bytes() == payload.size(),
      "virtual mDNS multicast delivery failed");

  const auto datagram = receiver->receive(payload.size());
  require(datagram &&
              datagram->bytes ==
                  std::vector<std::byte>{payload.begin(), payload.end()} &&
              datagram->source_address[darwin::network::sockaddr_port_offset] ==
                  local[darwin::network::sockaddr_port_offset],
          "virtual mDNS datagram/source address was not preserved");

  const auto control = bsd::make_virtual_udp_ancillary(
      darwin::network::address_family_inet, *datagram,
      bsd::VirtualUdpAncillaryOptions{true, true, true,
                                      virtual_interface_index});
  require(control.size() == expected_ipv4_control_size &&
              control[darwin::network::arm32_control_level_offset] ==
                  static_cast<std::byte>(darwin::network::protocol_ip) &&
              control[darwin::network::arm32_control_type_offset] ==
                  static_cast<std::byte>(
                      darwin::network::ip_receive_destination_address),
          "virtual mDNS recvmsg ancillary layout changed");

  constexpr std::uint32_t base = 0x4b000;
  constexpr std::uint32_t socket_address = base;
  constexpr std::uint32_t destination_address = base + 0x10;
  constexpr std::uint32_t disconnect_address = base + 0x20;
  constexpr std::uint32_t message_address = base + 0x40;
  constexpr std::uint32_t iovec_address = base + 0x80;
  constexpr std::uint32_t payload_address = base + 0x100;
  constexpr std::uint32_t peer_address = base + 0x140;
  constexpr std::uint32_t peer_length_address = base + 0x160;
  constexpr std::uint32_t receive_address = base + 0x180;
  constexpr std::uint32_t source_address = base + 0x1c0;
  constexpr std::uint32_t source_length_address = base + 0x1e0;

  AddressSpace memory;
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write) &&
              memory.copy_in(socket_address, local) &&
              memory.copy_in(destination_address, destination) &&
              memory.copy_in(payload_address, payload),
          "connected UDP guest memory setup failed");
  auto disconnected = local;
  disconnected[darwin::network::sockaddr_family_offset] = std::byte{0};
  require(memory.copy_in(disconnect_address, disconnected),
          "connected UDP disconnect address setup failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};
  const auto invoke = [&](std::uint32_t syscall) {
    cpu.registers()[12] = syscall;
    kernel.dispatch(cpu, 0x80);
  };
  const auto create_socket = [&] {
    cpu.registers()[0] = darwin::network::address_family_inet;
    cpu.registers()[1] = darwin::socket::datagram;
    cpu.registers()[2] = darwin::network::protocol_udp;
    invoke(darwin::syscall::socket);
    return cpu.registers()[0];
  };
  const auto sender_fd = create_socket();
  const auto receiver_fd = create_socket();
  require(sender_fd >= 3 && receiver_fd > sender_fd,
          "connected UDP syscall socket creation failed");

  cpu.registers()[0] = receiver_fd;
  cpu.registers()[1] = socket_address;
  cpu.registers()[2] = destination.size();
  invoke(darwin::syscall::bind);
  require(cpu.registers()[0] == 0, "connected UDP receiver bind failed");
  cpu.registers()[0] = sender_fd;
  cpu.registers()[1] = destination_address;
  cpu.registers()[2] = destination.size();
  invoke(darwin::syscall::connect);
  require(cpu.registers()[0] == 0, "virtual UDP connect failed");

  require(memory.write32(peer_length_address, destination.size()),
          "virtual UDP peer capacity setup failed");
  cpu.registers()[0] = sender_fd;
  cpu.registers()[1] = peer_address;
  cpu.registers()[2] = peer_length_address;
  invoke(darwin::syscall::get_peer_name);
  const auto copied_peer = memory.read_bytes(peer_address, destination.size());
  require(cpu.registers()[0] == 0 && copied_peer &&
              *copied_peer == std::vector<std::byte>{destination.begin(),
                                                     destination.end()},
          "virtual UDP getpeername did not preserve the connected peer");

  require(
      memory.write32(iovec_address, payload_address) &&
          memory.write32(iovec_address + 4, payload.size()) &&
          memory.write32(message_address +
                             darwin::socket::arm32_message::name_offset,
                         0) &&
          memory.write32(message_address +
                             darwin::socket::arm32_message::name_length_offset,
                         0) &&
          memory.write32(message_address +
                             darwin::socket::arm32_message::iov_offset,
                         iovec_address) &&
          memory.write32(message_address +
                             darwin::socket::arm32_message::iov_count_offset,
                         1) &&
          memory.write32(message_address +
                             darwin::socket::arm32_message::control_offset,
                         0) &&
          memory.write32(
              message_address +
                  darwin::socket::arm32_message::control_length_offset,
              0),
      "virtual UDP sendmsg structure setup failed");
  cpu.registers()[0] = sender_fd;
  cpu.registers()[1] = message_address;
  cpu.registers()[2] = 0;
  invoke(darwin::syscall::send_message);
  require(cpu.registers()[0] == payload.size(),
          "connected virtual UDP sendmsg failed");

  require(memory.write32(source_length_address, destination.size()),
          "connected UDP source capacity setup failed");
  cpu.registers()[0] = receiver_fd;
  cpu.registers()[1] = receive_address;
  cpu.registers()[2] = payload.size();
  cpu.registers()[3] = 0;
  cpu.registers()[4] = source_address;
  cpu.registers()[5] = source_length_address;
  invoke(darwin::syscall::receive_from);
  const auto received_payload =
      memory.read_bytes(receive_address, payload.size());
  require(cpu.registers()[0] == payload.size() && received_payload &&
              *received_payload ==
                  std::vector<std::byte>{payload.begin(), payload.end()},
          "connected virtual UDP payload did not reach recvfrom");

  cpu.registers()[0] = sender_fd;
  cpu.registers()[1] = disconnect_address;
  cpu.registers()[2] = disconnected.size();
  invoke(darwin::syscall::connect);
  require(cpu.registers()[0] == 0, "virtual UDP AF_UNSPEC disconnect failed");
  cpu.registers()[0] = sender_fd;
  cpu.registers()[1] = message_address;
  cpu.registers()[2] = 0;
  invoke(darwin::syscall::send_message);
  require(cpu.registers()[0] == darwin::error::not_connected,
          "disconnected virtual UDP sendmsg did not return ENOTCONN");
}

} // namespace

void run_virtual_udp_tests() { mdns_multicast_data_plane_test(); }

} // namespace ilemu::test::network_suite
