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

void host_network_loopback_test() {
  using namespace darwin::network;
  const auto resolver =
      parse_host_ipv4_resolver("# generated\nnameserver 2001:db8::53\n"
                               "nameserver 192.0.2.53 # primary IPv4\n");
  require(resolver == std::optional{std::array<std::byte, 4>{
                          std::byte{192}, std::byte{0}, std::byte{2},
                          std::byte{53}}} &&
              !parse_host_ipv4_resolver("search example.test\n"),
          "host resolver configuration parsing failed");
  require(parse_host_network_policy("isolated") ==
                  std::optional{HostNetworkPolicy::Isolated} &&
              parse_host_network_policy("loopback") ==
                  std::optional{HostNetworkPolicy::Loopback} &&
              parse_host_network_policy("host") ==
                  std::optional{HostNetworkPolicy::Host} &&
              !parse_host_network_policy("invalid"),
          "host-network policy parser accepted an invalid mode");

  const auto isolated = HostSocket::create(HostNetworkPolicy::Isolated,
                                           address_family_inet, 2, 0);
  require(!isolated.socket && isolated.darwin_error == 47,
          "isolated policy created a host socket");

  std::array<std::byte, 16> wildcard{};
  wildcard[0] = std::byte{16};
  wildcard[1] = std::byte{address_family_inet};
  const auto udp_server = HostSocket::create(HostNetworkPolicy::Loopback,
                                             address_family_inet, 2, 0);
  const auto udp_client = HostSocket::create(HostNetworkPolicy::Loopback,
                                             address_family_inet, 2, 0);
  if (!udp_server.socket || !udp_client.socket) {
    const auto sandbox_denied = [](std::uint32_t error) {
      return error == 1 || error == 13; // EPERM / EACCES
    };
    require((!udp_server.socket && sandbox_denied(udp_server.darwin_error)) ||
                (!udp_client.socket && sandbox_denied(udp_client.darwin_error)),
            "loopback UDP host socket failed for a non-policy reason");
    // Some hermetic test runners deny socket(2), including AF_INET
    // loopback. The same test is executed in network-enabled CI; all pure
    // sockaddr/policy checks above remain mandatory here.
    return;
  }
  require(udp_server.socket && udp_client.socket,
          "loopback UDP host socket creation failed");
  require(udp_server.socket->bind(wildcard).status == HostSocketStatus::Success,
          "loopback UDP bind failed");
  const auto udp_address = udp_server.socket->local_address();
  require(udp_address.status == HostSocketStatus::Success &&
              udp_address.address.size() == 16 &&
              udp_address.address[4] == std::byte{127} &&
              udp_address.address[7] == std::byte{1},
          "loopback policy exposed an INADDR_ANY listener");

  std::array<std::byte, 16> external{};
  external[0] = std::byte{16};
  external[1] = std::byte{address_family_inet};
  external[2] = std::byte{0};
  external[3] = std::byte{53};
  external[4] = std::byte{8};
  external[5] = std::byte{8};
  external[6] = std::byte{8};
  external[7] = std::byte{8};
  const std::array<std::byte, 4> udp_payload{std::byte{'u'}, std::byte{'d'},
                                             std::byte{'p'}, std::byte{'!'}};
  const auto denied = udp_client.socket->send(udp_payload, external);
  require(denied.status == HostSocketStatus::Error && denied.darwin_error == 65,
          "loopback policy allowed an external UDP destination");

  const auto udp_sent =
      udp_client.socket->send(udp_payload, udp_address.address);
  require(udp_sent.status == HostSocketStatus::Success &&
              udp_sent.transferred == udp_payload.size(),
          "loopback UDP send failed");
  HostSocketResult udp_pending;
  for (std::size_t attempt = 0; attempt < 10'000; ++attempt) {
    udp_pending = udp_server.socket->pending_bytes();
    require(udp_pending.status == HostSocketStatus::Success,
            "host FIONREAD query failed");
    if (udp_pending.transferred == udp_payload.size())
      break;
    std::this_thread::yield();
  }
  require(udp_pending.transferred == udp_payload.size(),
          "host pending-byte query did not report the queued datagram");
  const auto udp_received = udp_server.socket->receive(64);
  require(udp_received.status == HostSocketStatus::Success &&
              udp_received.bytes == std::vector<std::byte>{udp_payload.begin(),
                                                           udp_payload.end()} &&
              udp_received.address.size() == 16,
          "loopback UDP receive or Darwin source address conversion failed");

  // Exercise the BSD compatibility boundary as well as the standalone host
  // adapter. A blocking guest recvfrom must become an XNU-style kernel wait,
  // then resume only after the non-blocking host socket reports readiness.
  AddressSpace guest_memory;
  constexpr std::uint32_t guest_base = 0x5c000;
  constexpr std::uint32_t destination_address = guest_base;
  constexpr std::uint32_t guest_payload_address = guest_base + 0x40;
  constexpr std::uint32_t guest_local_address = guest_base + 0x80;
  constexpr std::uint32_t guest_local_length = guest_base + 0xc0;
  constexpr std::uint32_t guest_receive_address = guest_base + 0x100;
  constexpr std::uint32_t guest_source_address = guest_base + 0x140;
  constexpr std::uint32_t guest_source_length = guest_base + 0x180;
  constexpr std::uint32_t guest_write_set = guest_base + 0x1c0;
  constexpr std::uint32_t guest_kevent_change = guest_base + 0x200;
  constexpr std::uint32_t guest_kevent_output = guest_base + 0x240;
  constexpr std::uint32_t guest_message = guest_base + 0x280;
  constexpr std::uint32_t guest_iovecs = guest_base + 0x2c0;
  constexpr std::uint32_t guest_message_part_one = guest_base + 0x300;
  constexpr std::uint32_t guest_message_part_two = guest_base + 0x340;
  constexpr std::uint32_t guest_message_source = guest_base + 0x380;
  constexpr std::uint32_t guest_message_control = guest_base + 0x3c0;
  constexpr std::uint32_t second_guest_message = guest_base + 0x400;
  constexpr std::uint32_t second_guest_iovec = guest_base + 0x440;
  constexpr std::uint32_t second_guest_message_data = guest_base + 0x480;
  constexpr std::uint32_t second_guest_message_source = guest_base + 0x4c0;
  constexpr std::uint32_t guest_option_value = guest_base + 0x500;
  constexpr std::uint32_t guest_option_size = guest_base + 0x504;
  require(guest_memory.map(guest_base, AddressSpace::page_size,
                           MemoryPermission::Read | MemoryPermission::Write) &&
              guest_memory.copy_in(destination_address, udp_address.address) &&
              guest_memory.copy_in(guest_payload_address, udp_payload) &&
              guest_memory.write32(guest_local_length, 16) &&
              guest_memory.write32(guest_source_length, 16),
          "guest loopback socket memory setup failed");
  Dynarmic::ExclusiveMonitor guest_monitor{2};
  Cpu guest_cpu{0, guest_memory, guest_monitor};
  Cpu second_guest_cpu{1, guest_memory, guest_monitor};
  std::ostringstream guest_stream;
  Output guest_output{guest_stream};
  CompatibilityKernel guest_kernel{guest_memory, guest_output};
  guest_kernel.set_host_network_policy(HostNetworkPolicy::Loopback);

  guest_cpu.registers()[0] = address_family_inet;
  guest_cpu.registers()[1] = darwin::socket::datagram;
  guest_cpu.registers()[2] = 0;
  guest_cpu.registers()[12] = darwin::syscall::socket;
  guest_kernel.dispatch(guest_cpu, 0x80);
  const auto guest_fd = guest_cpu.registers()[0];
  require(guest_fd >= 3, "guest BSD socket did not create a host adapter");

  require(guest_memory.write32(guest_option_value, 1),
          "guest packet-info option value setup failed");
  for (const auto option : {darwin::network::ip_receive_destination_address,
                            darwin::network::ip_receive_interface,
                            darwin::network::ip_receive_ttl}) {
    guest_cpu.registers()[0] = guest_fd;
    guest_cpu.registers()[1] = darwin::network::protocol_ip;
    guest_cpu.registers()[2] = option;
    guest_cpu.registers()[3] = guest_option_value;
    guest_cpu.registers()[4] = sizeof(std::uint32_t);
    guest_cpu.registers()[12] = 105; // setsockopt
    guest_kernel.dispatch(guest_cpu, 0x80);
    require(guest_cpu.registers()[0] == 0,
            "guest mDNS packet-info setsockopt failed");
  }

  require(guest_fd < 32 &&
              guest_memory.write32(guest_write_set, 1U << guest_fd),
          "guest socket write fd_set setup failed");
  guest_cpu.registers()[0] = guest_fd + 1U;
  guest_cpu.registers()[1] = 0;
  guest_cpu.registers()[2] = guest_write_set;
  guest_cpu.registers()[3] = 0;
  guest_cpu.registers()[4] = 0;
  guest_cpu.registers()[12] = 93; // select
  guest_kernel.dispatch(guest_cpu, 0x80);
  require(guest_cpu.registers()[0] == 1 &&
              guest_memory.read32(guest_write_set) ==
                  std::optional<std::uint32_t>{1U << guest_fd},
          "select did not expose host-socket write readiness");

  guest_cpu.registers()[12] = 362; // kqueue
  guest_kernel.dispatch(guest_cpu, 0x80);
  const auto guest_kqueue = guest_cpu.registers()[0];
  require(guest_memory.write32(guest_kevent_change, guest_fd) &&
              guest_memory.write16(guest_kevent_change + 4, 0xfffeU) &&
              guest_memory.write16(guest_kevent_change + 6, 1) &&
              guest_memory.write32(guest_kevent_change + 8, 0) &&
              guest_memory.write32(guest_kevent_change + 12, 0) &&
              guest_memory.write32(guest_kevent_change + 16, 0x5678),
          "guest EVFILT_WRITE setup failed");
  guest_cpu.registers()[0] = guest_kqueue;
  guest_cpu.registers()[1] = guest_kevent_change;
  guest_cpu.registers()[2] = 1;
  guest_cpu.registers()[3] = guest_kevent_output;
  guest_cpu.registers()[4] = 1;
  guest_cpu.registers()[5] = 0;
  guest_cpu.registers()[12] = 363; // kevent
  guest_kernel.dispatch(guest_cpu, 0x80);
  require(guest_cpu.registers()[0] == 1 &&
              guest_memory.read32(guest_kevent_output) ==
                  std::optional<std::uint32_t>{guest_fd} &&
              guest_memory.read16(guest_kevent_output + 4) ==
                  std::optional<std::uint16_t>{0xfffeU} &&
              guest_memory.read32(guest_kevent_output + 16) ==
                  std::optional<std::uint32_t>{0x5678},
          "EVFILT_WRITE did not expose host-socket readiness");

  guest_cpu.registers()[0] = guest_fd;
  guest_cpu.registers()[1] = guest_payload_address;
  guest_cpu.registers()[2] = static_cast<std::uint32_t>(udp_payload.size());
  guest_cpu.registers()[3] = 0;
  guest_cpu.registers()[4] = destination_address;
  guest_cpu.registers()[5] = 16;
  guest_cpu.registers()[12] = darwin::syscall::send_to;
  guest_kernel.dispatch(guest_cpu, 0x80);
  require(guest_cpu.registers()[0] == udp_payload.size(),
          "guest BSD sendto did not reach the host UDP socket");
  HostSocketResult guest_datagram;
  for (std::size_t attempt = 0; attempt < 10'000; ++attempt) {
    guest_datagram = udp_server.socket->receive(64);
    if (guest_datagram.status == HostSocketStatus::Success)
      break;
    require(guest_datagram.status == HostSocketStatus::WouldBlock,
            "host receive of guest UDP payload failed");
    std::this_thread::yield();
  }
  require(
      guest_datagram.status == HostSocketStatus::Success &&
          guest_datagram.bytes ==
              std::vector<std::byte>{udp_payload.begin(), udp_payload.end()},
      "guest BSD sendto payload mismatch");

  guest_cpu.registers()[0] = guest_fd;
  guest_cpu.registers()[1] = guest_local_address;
  guest_cpu.registers()[2] = guest_local_length;
  guest_cpu.registers()[12] = 32; // getsockname
  guest_kernel.dispatch(guest_cpu, 0x80);
  const auto local_length = guest_memory.read32(guest_local_length);
  const auto local_address =
      local_length ? guest_memory.read_bytes(guest_local_address, *local_length)
                   : std::nullopt;
  require(guest_cpu.registers()[0] == 0 && local_address &&
              local_address->size() == 16,
          "guest BSD getsockname did not return a Darwin sockaddr");

  guest_cpu.registers()[0] = guest_fd;
  guest_cpu.registers()[1] = guest_receive_address;
  guest_cpu.registers()[2] = 64;
  guest_cpu.registers()[3] = 0;
  guest_cpu.registers()[4] = guest_source_address;
  guest_cpu.registers()[5] = guest_source_length;
  guest_cpu.registers()[12] = darwin::syscall::receive_from;
  guest_kernel.dispatch(guest_cpu, 0x80);
  require(guest_kernel.wait_reason(0) ==
              "read(fd=" + std::to_string(guest_fd) + ")",
          "blocking guest recvfrom bypassed the XNU wait path");

  const std::array<std::byte, 4> response{std::byte{'p'}, std::byte{'o'},
                                          std::byte{'n'}, std::byte{'g'}};
  const auto response_sent =
      udp_server.socket->send(response, guest_datagram.address);
  require(response_sent.status == HostSocketStatus::Success,
          "host UDP response to guest failed");
  std::uint32_t guest_pending_bytes = 0;
  for (std::size_t attempt = 0; attempt < 10'000; ++attempt) {
    require(guest_memory.write32(guest_option_size, sizeof(std::uint32_t)),
            "guest SO_NREAD size setup failed");
    second_guest_cpu.registers()[0] = guest_fd;
    second_guest_cpu.registers()[1] = darwin::socket::option_level;
    second_guest_cpu.registers()[2] = darwin::socket::option_pending_bytes;
    second_guest_cpu.registers()[3] = guest_option_value;
    second_guest_cpu.registers()[4] = guest_option_size;
    second_guest_cpu.registers()[12] = 118; // getsockopt
    guest_kernel.dispatch(second_guest_cpu, 0x80);
    require(second_guest_cpu.registers()[0] == 0,
            "guest host-socket SO_NREAD failed");
    guest_pending_bytes = guest_memory.read32(guest_option_value).value_or(0);
    if (guest_pending_bytes == response.size())
      break;
    std::this_thread::yield();
  }
  require(guest_pending_bytes == response.size(),
          "guest SO_NREAD did not report the queued host datagram");
  second_guest_cpu.registers()[0] = guest_fd;
  second_guest_cpu.registers()[1] = darwin::socket::ioctl_pending_bytes;
  second_guest_cpu.registers()[2] = guest_option_value;
  second_guest_cpu.registers()[12] = 54; // ioctl(FIONREAD)
  guest_kernel.dispatch(second_guest_cpu, 0x80);
  require(second_guest_cpu.registers()[0] == 0 &&
              guest_memory.read32(guest_option_value) ==
                  std::optional<std::uint32_t>{response.size()},
          "guest FIONREAD did not report the queued host datagram");
  bool guest_woken = false;
  for (std::size_t attempt = 0; attempt < 10'000 && !guest_woken; ++attempt) {
    guest_woken = guest_kernel.deliver_pending_io(guest_cpu);
    if (!guest_woken)
      std::this_thread::yield();
  }
  const auto received_response =
      guest_memory.read_bytes(guest_receive_address, response.size());
  require(guest_woken && guest_cpu.registers()[0] == response.size() &&
              received_response &&
              *received_response ==
                  std::vector<std::byte>{response.begin(), response.end()} &&
              guest_memory.read32(guest_source_length) ==
                  std::optional<std::uint32_t>{16},
          "host readiness did not wake guest recvfrom with source address");

  // mDNSResponder uses recvmsg rather than recvfrom. Verify the Darwin
  // ARM32 msghdr/iovec translation, scatter copy, source sockaddr result,
  // empty host ancillary result, and the same scheduler wait/wakeup path.
  using namespace darwin::socket;
  require(
      guest_memory.write32(guest_message + arm32_message::name_offset,
                           guest_message_source) &&
          guest_memory.write32(
              guest_message + arm32_message::name_length_offset, 16) &&
          guest_memory.write32(guest_message + arm32_message::iov_offset,
                               guest_iovecs) &&
          guest_memory.write32(guest_message + arm32_message::iov_count_offset,
                               2) &&
          guest_memory.write32(guest_message + arm32_message::control_offset,
                               guest_message_control) &&
          guest_memory.write32(
              guest_message + arm32_message::control_length_offset, 64) &&
          guest_memory.write32(guest_message + arm32_message::flags_offset,
                               0xffffffffU) &&
          guest_memory.write32(guest_iovecs + arm32_iovec::base_offset,
                               guest_message_part_one) &&
          guest_memory.write32(guest_iovecs + arm32_iovec::length_offset, 2) &&
          guest_memory.write32(guest_iovecs + arm32_iovec::size +
                                   arm32_iovec::base_offset,
                               guest_message_part_two) &&
          guest_memory.write32(guest_iovecs + arm32_iovec::size +
                                   arm32_iovec::length_offset,
                               6) &&
          guest_memory.write32(second_guest_message +
                                   arm32_message::name_offset,
                               second_guest_message_source) &&
          guest_memory.write32(
              second_guest_message + arm32_message::name_length_offset, 16) &&
          guest_memory.write32(second_guest_message + arm32_message::iov_offset,
                               second_guest_iovec) &&
          guest_memory.write32(
              second_guest_message + arm32_message::iov_count_offset, 1) &&
          guest_memory.write32(
              second_guest_message + arm32_message::control_offset, 0) &&
          guest_memory.write32(
              second_guest_message + arm32_message::control_length_offset, 0) &&
          guest_memory.write32(
              second_guest_message + arm32_message::flags_offset, 0) &&
          guest_memory.write32(second_guest_iovec + arm32_iovec::base_offset,
                               second_guest_message_data) &&
          guest_memory.write32(second_guest_iovec + arm32_iovec::length_offset,
                               8),
      "guest recvmsg ARM32 structure setup failed");
  guest_cpu.registers()[0] = guest_fd;
  guest_cpu.registers()[1] = guest_message;
  guest_cpu.registers()[2] = 0;
  guest_cpu.registers()[12] = darwin::syscall::receive_message;
  guest_kernel.dispatch(guest_cpu, 0x80);
  require(guest_kernel.wait_reason(0) ==
              "recvmsg(fd=" + std::to_string(guest_fd) + ")",
          "blocking host recvmsg bypassed the XNU wait path");
  second_guest_cpu.registers()[0] = guest_fd;
  second_guest_cpu.registers()[1] = second_guest_message;
  second_guest_cpu.registers()[2] = 0;
  second_guest_cpu.registers()[12] = darwin::syscall::receive_message;
  guest_kernel.dispatch(second_guest_cpu, 0x80);
  require(guest_kernel.wait_reason(1) ==
              "recvmsg(fd=" + std::to_string(guest_fd) + ")",
          "second guest recvmsg overwrote the first thread's wait state");

  const std::array<std::byte, 6> message_response{
      std::byte{'m'}, std::byte{'d'}, std::byte{'n'},
      std::byte{'s'}, std::byte{'!'}, std::byte{'!'}};
  require(udp_server.socket->send(message_response, guest_datagram.address)
                  .status == HostSocketStatus::Success,
          "host UDP recvmsg response to guest failed");
  bool message_woken = false;
  for (std::size_t attempt = 0; attempt < 10'000 && !message_woken; ++attempt) {
    message_woken = guest_kernel.deliver_pending_io(guest_cpu);
    if (!message_woken)
      std::this_thread::yield();
  }
  const auto message_part_one =
      guest_memory.read_bytes(guest_message_part_one, 2);
  const auto message_part_two =
      guest_memory.read_bytes(guest_message_part_two, 4);
  require(
      message_woken && guest_cpu.registers()[0] == message_response.size() &&
          message_part_one && message_part_two &&
          *message_part_one ==
              std::vector<std::byte>{message_response.begin(),
                                     message_response.begin() + 2} &&
          *message_part_two ==
              std::vector<std::byte>{message_response.begin() + 2,
                                     message_response.end()} &&
          guest_memory.read8(guest_message_source + 1) ==
              std::optional<std::uint8_t>{address_family_inet} &&
          guest_memory.read32(guest_message +
                              arm32_message::name_length_offset) ==
              std::optional<std::uint32_t>{16} &&
          guest_memory.read32(guest_message +
                              arm32_message::control_length_offset) ==
              std::optional<std::uint32_t>{64} &&
          guest_memory.read32(guest_message + arm32_message::flags_offset) ==
              std::optional<std::uint32_t>{0},
      "host recvmsg did not scatter payload or return Darwin metadata");
  require(
      guest_memory.read32(guest_message_control) ==
              std::optional<std::uint32_t>{16} &&
          guest_memory.read32(guest_message_control + 4) ==
              std::optional<std::uint32_t>{darwin::network::protocol_ip} &&
          guest_memory.read32(guest_message_control + 8) ==
              std::optional<std::uint32_t>{
                  darwin::network::ip_receive_destination_address} &&
          guest_memory.read_bytes(guest_message_control + 12, 4) ==
              std::optional<std::vector<std::byte>>{std::vector<std::byte>{
                  std::byte{127}, std::byte{0}, std::byte{0}, std::byte{1}}} &&
          guest_memory.read32(guest_message_control + 16) ==
              std::optional<std::uint32_t>{32} &&
          guest_memory.read32(guest_message_control + 20) ==
              std::optional<std::uint32_t>{darwin::network::protocol_ip} &&
          guest_memory.read32(guest_message_control + 24) ==
              std::optional<std::uint32_t>{
                  darwin::network::ip_receive_interface} &&
          guest_memory.read8(guest_message_control + 28) ==
              std::optional<std::uint8_t>{20} &&
          guest_memory.read8(guest_message_control + 29) ==
              std::optional<std::uint8_t>{
                  darwin::network::address_family_link} &&
          guest_memory.read16(guest_message_control + 30) ==
              std::optional<std::uint16_t>{1} &&
          guest_memory.read_bytes(guest_message_control + 36, 3) ==
              std::optional<std::vector<std::byte>>{std::vector<std::byte>{
                  std::byte{'l'}, std::byte{'o'}, std::byte{'0'}}} &&
          guest_memory.read32(guest_message_control + 48) ==
              std::optional<std::uint32_t>{13} &&
          guest_memory.read32(guest_message_control + 52) ==
              std::optional<std::uint32_t>{darwin::network::protocol_ip} &&
          guest_memory.read32(guest_message_control + 56) ==
              std::optional<std::uint32_t>{darwin::network::ip_receive_ttl},
      "host recvmsg did not return Darwin mDNS ancillary controls");
  require(guest_kernel.wait_reason(1) ==
              "recvmsg(fd=" + std::to_string(guest_fd) + ")",
          "waking one recvmsg incorrectly cleared another thread's wait");

  const std::array<std::byte, 4> second_message_response{
      std::byte{'d'}, std::byte{'n'}, std::byte{'s'}, std::byte{'2'}};
  require(
      udp_server.socket->send(second_message_response, guest_datagram.address)
              .status == HostSocketStatus::Success,
      "second host UDP recvmsg response to guest failed");
  bool second_message_woken = false;
  for (std::size_t attempt = 0; attempt < 10'000 && !second_message_woken;
       ++attempt) {
    second_message_woken = guest_kernel.deliver_pending_io(second_guest_cpu);
    if (!second_message_woken)
      std::this_thread::yield();
  }
  const auto second_message_data = guest_memory.read_bytes(
      second_guest_message_data, second_message_response.size());
  require(second_message_woken &&
              second_guest_cpu.registers()[0] ==
                  second_message_response.size() &&
              second_message_data &&
              *second_message_data ==
                  std::vector<std::byte>{second_message_response.begin(),
                                         second_message_response.end()} &&
              guest_kernel.wait_reason(1) == "none",
          "second guest thread did not retain and complete its recvmsg wait");

  const auto tcp_server = HostSocket::create(HostNetworkPolicy::Loopback,
                                             address_family_inet, 1, 0);
  const auto tcp_client = HostSocket::create(HostNetworkPolicy::Loopback,
                                             address_family_inet, 1, 0);
  require(tcp_server.socket && tcp_client.socket &&
              tcp_server.socket->bind(wildcard).status ==
                  HostSocketStatus::Success &&
              tcp_server.socket->listen(4).status == HostSocketStatus::Success,
          "loopback TCP listener setup failed");
  const auto tcp_address = tcp_server.socket->local_address();
  require(tcp_address.status == HostSocketStatus::Success,
          "loopback TCP listener address query failed");

  require(guest_memory.copy_in(destination_address, tcp_address.address),
          "guest TCP destination copy failed");
  guest_cpu.registers()[0] = address_family_inet;
  guest_cpu.registers()[1] = darwin::socket::stream;
  guest_cpu.registers()[2] = 0;
  guest_cpu.registers()[12] = darwin::syscall::socket;
  guest_kernel.dispatch(guest_cpu, 0x80);
  const auto guest_tcp_fd = guest_cpu.registers()[0];
  require(guest_tcp_fd > guest_fd, "guest TCP socket creation failed");

  guest_cpu.registers()[0] = guest_tcp_fd;
  guest_cpu.registers()[1] = destination_address;
  guest_cpu.registers()[2] =
      static_cast<std::uint32_t>(tcp_address.address.size());
  guest_cpu.registers()[12] = darwin::syscall::connect;
  guest_kernel.dispatch(guest_cpu, 0x80);
  const bool guest_connect_waiting =
      guest_kernel.wait_reason(0) ==
      "connect(fd=" + std::to_string(guest_tcp_fd) + ")";
  require(guest_connect_waiting || (guest_cpu.registers()[0] == 0 &&
                                    (guest_cpu.cpsr() & (1U << 29U)) == 0),
          "guest TCP connect failed instead of completing or waiting");

  std::shared_ptr<HostSocket> guest_tcp_peer;
  bool guest_tcp_connected = !guest_connect_waiting;
  for (std::size_t attempt = 0;
       attempt < 10'000 && (!guest_tcp_peer || !guest_tcp_connected);
       ++attempt) {
    if (!guest_tcp_peer) {
      const auto accepted_guest = tcp_server.socket->accept();
      if (accepted_guest.status == HostSocketStatus::Success) {
        guest_tcp_peer = accepted_guest.accepted_socket;
      } else {
        require(accepted_guest.status == HostSocketStatus::WouldBlock,
                "host listener failed while accepting guest TCP");
      }
    }
    if (!guest_tcp_connected && guest_kernel.deliver_pending_io(guest_cpu)) {
      guest_tcp_connected = guest_cpu.registers()[0] == 0 &&
                            (guest_cpu.cpsr() & (1U << 29U)) == 0;
    }
    std::this_thread::yield();
  }
  require(guest_tcp_peer && guest_tcp_connected,
          "guest TCP connect did not complete through scheduler wakeup");

  const std::array<std::byte, 4> guest_tcp_payload{
      std::byte{'g'}, std::byte{'u'}, std::byte{'e'}, std::byte{'s'}};
  require(guest_memory.copy_in(guest_payload_address, guest_tcp_payload),
          "guest TCP payload copy failed");
  guest_cpu.registers()[0] = guest_tcp_fd;
  guest_cpu.registers()[1] = guest_payload_address;
  guest_cpu.registers()[2] =
      static_cast<std::uint32_t>(guest_tcp_payload.size());
  guest_cpu.registers()[12] = darwin::syscall::write;
  guest_kernel.dispatch(guest_cpu, 0x80);
  require(guest_cpu.registers()[0] == guest_tcp_payload.size(),
          "guest TCP write failed");
  HostSocketResult guest_tcp_received;
  for (std::size_t attempt = 0; attempt < 10'000; ++attempt) {
    guest_tcp_received = guest_tcp_peer->receive(64);
    if (guest_tcp_received.status == HostSocketStatus::Success)
      break;
    require(guest_tcp_received.status == HostSocketStatus::WouldBlock,
            "guest TCP peer receive failed");
    std::this_thread::yield();
  }
  require(guest_tcp_received.status == HostSocketStatus::Success &&
              guest_tcp_received.bytes ==
                  std::vector<std::byte>{guest_tcp_payload.begin(),
                                         guest_tcp_payload.end()},
          "guest TCP payload mismatch");

  const auto connect = tcp_client.socket->connect(tcp_address.address);
  require(connect.status != HostSocketStatus::Error,
          "loopback TCP connect failed immediately");

  std::shared_ptr<HostSocket> accepted;
  bool connected = connect.status == HostSocketStatus::Success;
  for (std::size_t attempt = 0; attempt < 10'000 && (!accepted || !connected);
       ++attempt) {
    if (!accepted) {
      const auto result = tcp_server.socket->accept();
      if (result.status == HostSocketStatus::Success) {
        accepted = result.accepted_socket;
      } else {
        require(result.status == HostSocketStatus::WouldBlock,
                "loopback TCP accept returned an unexpected error");
      }
    }
    if (!connected) {
      const auto result = tcp_client.socket->finish_connect();
      if (result.status == HostSocketStatus::Success) {
        connected = true;
      } else {
        require(result.status == HostSocketStatus::WouldBlock,
                "loopback TCP completion returned an unexpected error");
      }
    }
    std::this_thread::yield();
  }
  require(accepted && connected,
          "loopback TCP connection did not become ready");

  const std::array<std::byte, 4> tcp_payload{std::byte{'t'}, std::byte{'c'},
                                             std::byte{'p'}, std::byte{'!'}};
  const auto tcp_sent = tcp_client.socket->send(tcp_payload);
  require(tcp_sent.status == HostSocketStatus::Success &&
              tcp_sent.transferred == tcp_payload.size(),
          "loopback TCP send failed");
  HostSocketResult tcp_received;
  for (std::size_t attempt = 0; attempt < 10'000; ++attempt) {
    tcp_received = accepted->receive(64);
    if (tcp_received.status == HostSocketStatus::Success)
      break;
    require(tcp_received.status == HostSocketStatus::WouldBlock,
            "loopback TCP receive returned an unexpected error");
    std::this_thread::yield();
  }
  require(tcp_received.status == HostSocketStatus::Success &&
              tcp_received.bytes == std::vector<std::byte>{tcp_payload.begin(),
                                                           tcp_payload.end()},
          "loopback TCP payload mismatch");
}

void mdns_host_socket_options_test() {
  using namespace darwin;
  const std::array<std::byte, 4> enabled{std::byte{1}, std::byte{0},
                                         std::byte{0}, std::byte{0}};
  const std::array<std::byte, 4> maximum_hops{std::byte{255}, std::byte{0},
                                              std::byte{0}, std::byte{0}};
  const std::array<std::byte, 4> loopback_v4{std::byte{127}, std::byte{0},
                                             std::byte{0}, std::byte{1}};
  std::array<std::byte, 16> wildcard_v4{};
  wildcard_v4[0] = std::byte{16};
  wildcard_v4[1] = std::byte{network::address_family_inet};

  const auto first_v4 =
      HostSocket::create(HostNetworkPolicy::Loopback,
                         network::address_family_inet, socket::datagram, 0);
  const auto second_v4 =
      HostSocket::create(HostNetworkPolicy::Loopback,
                         network::address_family_inet, socket::datagram, 0);
  if (!first_v4.socket || !second_v4.socket) {
    const auto denied = [](std::uint32_t error) {
      return error == 1 || error == 13;
    };
    require((!first_v4.socket && denied(first_v4.darwin_error)) ||
                (!second_v4.socket && denied(second_v4.darwin_error)),
            "mDNS IPv4 option socket creation failed");
    return;
  }
  require(first_v4.socket
                      ->set_option(socket::option_level,
                                   socket::option_reuse_port, enabled)
                      .status == HostSocketStatus::Success &&
              second_v4.socket
                      ->set_option(socket::option_level,
                                   socket::option_reuse_port, enabled)
                      .status == HostSocketStatus::Success &&
              first_v4.socket->bind(wildcard_v4).status ==
                  HostSocketStatus::Success,
          "mDNS SO_REUSEPORT setup failed");
  const auto shared_v4_address = first_v4.socket->local_address();
  require(shared_v4_address.status == HostSocketStatus::Success &&
              second_v4.socket->bind(shared_v4_address.address).status ==
                  HostSocketStatus::Success,
          "SO_REUSEPORT did not permit a second UDP listener");

  std::array<std::byte, network::ipv4_membership_size> membership_v4{
      std::byte{224}, std::byte{0}, std::byte{0}, std::byte{251},
      std::byte{127}, std::byte{0}, std::byte{0}, std::byte{1}};
  require(first_v4.socket
                      ->set_option(network::protocol_ip,
                                   network::ip_multicast_interface, loopback_v4)
                      .status == HostSocketStatus::Success &&
              first_v4.socket
                      ->set_option(network::protocol_ip,
                                   network::ip_multicast_ttl, maximum_hops)
                      .status == HostSocketStatus::Success &&
              first_v4.socket
                      ->set_option(network::protocol_ip,
                                   network::ip_multicast_loop, enabled)
                      .status == HostSocketStatus::Success &&
              first_v4.socket
                      ->set_option(network::protocol_ip,
                                   network::ip_add_membership, membership_v4)
                      .status == HostSocketStatus::Success &&
              first_v4.socket
                      ->set_option(network::protocol_ip,
                                   network::ip_drop_membership, membership_v4)
                      .status == HostSocketStatus::Success,
          "Darwin IPv4 multicast options were not translated to lo0");

  const auto ipv6_server =
      HostSocket::create(HostNetworkPolicy::Loopback,
                         network::address_family_inet6, socket::datagram, 0);
  const auto ipv6_client =
      HostSocket::create(HostNetworkPolicy::Loopback,
                         network::address_family_inet6, socket::datagram, 0);
  if (!ipv6_server.socket || !ipv6_client.socket) {
    require((!ipv6_server.socket &&
             (ipv6_server.darwin_error == 1 || ipv6_server.darwin_error == 13 ||
              ipv6_server.darwin_error == 47)) ||
                (!ipv6_client.socket && (ipv6_client.darwin_error == 1 ||
                                         ipv6_client.darwin_error == 13 ||
                                         ipv6_client.darwin_error == 47)),
            "mDNS IPv6 option socket creation failed");
    return;
  }
  std::array<std::byte, network::ipv6_membership_size> membership_v6{};
  membership_v6[0] = std::byte{0xff};
  membership_v6[1] = std::byte{0x02};
  membership_v6[15] = std::byte{0xfb};
  membership_v6[16] = std::byte{1}; // virtual lo0 index
  require(ipv6_server.socket
                      ->set_option(socket::option_level,
                                   socket::option_reuse_port, enabled)
                      .status == HostSocketStatus::Success &&
              ipv6_server.socket
                      ->set_option(network::protocol_ipv6,
                                   network::ipv6_packet_info, enabled)
                      .status == HostSocketStatus::Success &&
              ipv6_server.socket
                      ->set_option(network::protocol_ipv6,
                                   network::ipv6_hop_limit, enabled)
                      .status == HostSocketStatus::Success &&
              ipv6_server.socket
                      ->set_option(network::protocol_ipv6, network::ipv6_only,
                                   enabled)
                      .status == HostSocketStatus::Success &&
              ipv6_server.socket
                      ->set_option(network::protocol_ipv6,
                                   network::ipv6_multicast_interface, enabled)
                      .status == HostSocketStatus::Success &&
              ipv6_server.socket
                      ->set_option(network::protocol_ipv6,
                                   network::ipv6_multicast_hops, maximum_hops)
                      .status == HostSocketStatus::Success &&
              ipv6_server.socket
                      ->set_option(network::protocol_ipv6,
                                   network::ipv6_multicast_loop, enabled)
                      .status == HostSocketStatus::Success &&
              ipv6_server.socket
                      ->set_option(network::protocol_ipv6,
                                   network::ipv6_join_group, membership_v6)
                      .status == HostSocketStatus::Success &&
              ipv6_server.socket
                      ->set_option(network::protocol_ipv6,
                                   network::ipv6_leave_group, membership_v6)
                      .status == HostSocketStatus::Success,
          "Darwin IPv6 mDNS options were not translated to lo0");

  std::array<std::byte, 28> wildcard_v6{};
  wildcard_v6[0] = std::byte{28};
  wildcard_v6[1] = std::byte{network::address_family_inet6};
  require(ipv6_server.socket->bind(wildcard_v6).status ==
              HostSocketStatus::Success,
          "IPv6 mDNS loopback bind failed");
  const auto ipv6_address = ipv6_server.socket->local_address();
  const std::array<std::byte, 4> payload{std::byte{'v'}, std::byte{'6'},
                                         std::byte{'!'}, std::byte{'!'}};
  require(ipv6_address.status == HostSocketStatus::Success &&
              ipv6_address.address.size() == wildcard_v6.size() &&
              ipv6_client.socket->send(payload, ipv6_address.address).status ==
                  HostSocketStatus::Success,
          "IPv6 mDNS loopback send failed");
  HostSocketResult received;
  for (std::size_t attempt = 0; attempt < 10'000; ++attempt) {
    received = ipv6_server.socket->receive(64);
    if (received.status == HostSocketStatus::Success)
      break;
    require(received.status == HostSocketStatus::WouldBlock,
            "IPv6 mDNS loopback receive failed");
    std::this_thread::yield();
  }
  require(received.status == HostSocketStatus::Success &&
              received.bytes ==
                  std::vector<std::byte>{payload.begin(), payload.end()} &&
              received.destination_address.size() == 16 &&
              received.destination_address[15] == std::byte{1} &&
              received.interface_index == std::optional<std::uint32_t>{1} &&
              received.hop_limit.has_value(),
          "IPv6 pktinfo/hop-limit was not projected as virtual lo0 metadata");

  AddressSpace guest_memory;
  constexpr std::uint32_t guest_base = 0x7e000;
  constexpr std::uint32_t guest_option = guest_base;
  constexpr std::uint32_t guest_option_size = guest_base + 4;
  constexpr std::uint32_t guest_membership = guest_base + 0x20;
  constexpr std::uint32_t guest_bind_address = guest_base + 0x60;
  constexpr std::uint32_t guest_local_address = guest_base + 0xa0;
  constexpr std::uint32_t guest_local_length = guest_base + 0xc0;
  constexpr std::uint32_t guest_message = guest_base + 0xe0;
  constexpr std::uint32_t guest_iovec = guest_base + 0x120;
  constexpr std::uint32_t guest_payload = guest_base + 0x140;
  constexpr std::uint32_t guest_source = guest_base + 0x180;
  constexpr std::uint32_t guest_control = guest_base + 0x1c0;
  require(guest_memory.map(guest_base, AddressSpace::page_size,
                           MemoryPermission::Read | MemoryPermission::Write) &&
              guest_memory.copy_in(guest_bind_address, wildcard_v6) &&
              guest_memory.copy_in(guest_membership, membership_v6),
          "guest IPv6 mDNS memory setup failed");
  Dynarmic::ExclusiveMonitor guest_monitor{1};
  Cpu guest_cpu{0, guest_memory, guest_monitor};
  std::ostringstream guest_stream;
  Output guest_output{guest_stream};
  CompatibilityKernel guest_kernel{guest_memory, guest_output};
  guest_kernel.set_host_network_policy(HostNetworkPolicy::Loopback);

  guest_cpu.registers()[0] = network::address_family_inet6;
  guest_cpu.registers()[1] = socket::datagram;
  guest_cpu.registers()[2] = 0;
  guest_cpu.registers()[12] = syscall::socket;
  guest_kernel.dispatch(guest_cpu, 0x80);
  const auto guest_fd = guest_cpu.registers()[0];
  require(guest_fd >= 3, "guest IPv6 mDNS socket creation failed");

  const auto set_guest_option = [&](std::uint32_t level, std::uint32_t option,
                                    std::span<const std::byte> bytes,
                                    std::uint32_t address) {
    require(guest_memory.copy_in(address, bytes),
            "guest IPv6 option copy failed");
    guest_cpu.registers()[0] = guest_fd;
    guest_cpu.registers()[1] = level;
    guest_cpu.registers()[2] = option;
    guest_cpu.registers()[3] = address;
    guest_cpu.registers()[4] = static_cast<std::uint32_t>(bytes.size());
    guest_cpu.registers()[12] = syscall::set_socket_option;
    guest_kernel.dispatch(guest_cpu, 0x80);
    require(guest_cpu.registers()[0] == 0 &&
                (guest_cpu.cpsr() & (1U << 29U)) == 0,
            "guest IPv6 option translation failed");
  };
  set_guest_option(socket::option_level, socket::option_reuse_port, enabled,
                   guest_option);
  set_guest_option(network::protocol_ipv6, network::ipv6_packet_info, enabled,
                   guest_option);
  set_guest_option(network::protocol_ipv6, network::ipv6_hop_limit, enabled,
                   guest_option);
  set_guest_option(network::protocol_ipv6, network::ipv6_only, enabled,
                   guest_option);
  set_guest_option(network::protocol_ipv6, network::ipv6_multicast_interface,
                   enabled, guest_option);
  set_guest_option(network::protocol_ipv6, network::ipv6_multicast_hops,
                   maximum_hops, guest_option);
  set_guest_option(network::protocol_ipv6, network::ipv6_multicast_loop,
                   enabled, guest_option);
  set_guest_option(network::protocol_ipv6, network::ipv6_join_group,
                   membership_v6, guest_membership);
  set_guest_option(network::protocol_ipv6, network::ipv6_leave_group,
                   membership_v6, guest_membership);

  guest_cpu.registers()[0] = guest_fd;
  guest_cpu.registers()[1] = guest_bind_address;
  guest_cpu.registers()[2] = wildcard_v6.size();
  guest_cpu.registers()[12] = syscall::bind;
  guest_kernel.dispatch(guest_cpu, 0x80);
  require(guest_cpu.registers()[0] == 0 &&
              guest_memory.write32(guest_local_length, wildcard_v6.size()),
          "guest IPv6 mDNS bind failed");
  guest_cpu.registers()[0] = guest_fd;
  guest_cpu.registers()[1] = guest_local_address;
  guest_cpu.registers()[2] = guest_local_length;
  guest_cpu.registers()[12] = syscall::get_socket_name;
  guest_kernel.dispatch(guest_cpu, 0x80);
  const auto guest_destination =
      guest_memory.read_bytes(guest_local_address, wildcard_v6.size());
  require(guest_cpu.registers()[0] == 0 && guest_destination &&
              (*guest_destination)[1] ==
                  std::byte{network::address_family_inet6},
          "guest IPv6 mDNS local address query failed");

  require(guest_memory.write32(guest_option, 0xffff'ffffU) &&
              guest_memory.write32(guest_option_size, sizeof(std::uint32_t)),
          "guest SO_ERROR result storage setup failed");
  guest_cpu.registers()[0] = guest_fd;
  guest_cpu.registers()[1] = socket::option_level;
  guest_cpu.registers()[2] = socket::option_error;
  guest_cpu.registers()[3] = guest_option;
  guest_cpu.registers()[4] = guest_option_size;
  guest_cpu.registers()[12] = syscall::get_socket_option;
  guest_kernel.dispatch(guest_cpu, 0x80);
  require(guest_cpu.registers()[0] == 0 &&
              guest_memory.read32(guest_option) ==
                  std::optional<std::uint32_t>{0} &&
              guest_memory.read32(guest_option_size) ==
                  std::optional<std::uint32_t>{sizeof(std::uint32_t)},
          "guest SO_ERROR did not return the translated host socket state");

  require(
      guest_memory.write32(guest_message + socket::arm32_message::name_offset,
                           guest_source) &&
          guest_memory.write32(guest_message +
                                   socket::arm32_message::name_length_offset,
                               wildcard_v6.size()) &&
          guest_memory.write32(
              guest_message + socket::arm32_message::iov_offset, guest_iovec) &&
          guest_memory.write32(
              guest_message + socket::arm32_message::iov_count_offset, 1) &&
          guest_memory.write32(guest_message +
                                   socket::arm32_message::control_offset,
                               guest_control) &&
          guest_memory.write32(guest_message +
                                   socket::arm32_message::control_length_offset,
                               64) &&
          guest_memory.write32(guest_message +
                                   socket::arm32_message::flags_offset,
                               0xffff'ffffU) &&
          guest_memory.write32(guest_iovec + socket::arm32_iovec::base_offset,
                               guest_payload) &&
          guest_memory.write32(guest_iovec + socket::arm32_iovec::length_offset,
                               16),
      "guest IPv6 recvmsg ARM32 structure setup failed");
  guest_cpu.registers()[0] = guest_fd;
  guest_cpu.registers()[1] = guest_message;
  guest_cpu.registers()[2] = 0;
  guest_cpu.registers()[12] = syscall::receive_message;
  guest_kernel.dispatch(guest_cpu, 0x80);
  require(guest_kernel.wait_reason(0) ==
              "recvmsg(fd=" + std::to_string(guest_fd) + ")",
          "guest IPv6 recvmsg did not enter the scheduler wait path");

  const std::array<std::byte, 6> guest_response{std::byte{'m'}, std::byte{'D'},
                                                std::byte{'N'}, std::byte{'S'},
                                                std::byte{'6'}, std::byte{'!'}};
  require(ipv6_client.socket->send(guest_response, *guest_destination).status ==
              HostSocketStatus::Success,
          "host could not send the guest IPv6 mDNS response");
  bool guest_woken = false;
  for (std::size_t attempt = 0; attempt < 10'000 && !guest_woken; ++attempt) {
    guest_woken = guest_kernel.deliver_pending_io(guest_cpu);
    if (!guest_woken)
      std::this_thread::yield();
  }
  require(
      guest_woken && guest_cpu.registers()[0] == guest_response.size() &&
          guest_memory.read_bytes(guest_payload, guest_response.size()) ==
              std::optional<std::vector<std::byte>>{std::vector<std::byte>{
                  guest_response.begin(), guest_response.end()}} &&
          guest_memory.read8(guest_source + 1) ==
              std::optional<std::uint8_t>{network::address_family_inet6} &&
          guest_memory.read32(guest_message +
                              socket::arm32_message::control_length_offset) ==
              std::optional<std::uint32_t>{48} &&
          guest_memory.read32(guest_control) ==
              std::optional<std::uint32_t>{32} &&
          guest_memory.read32(guest_control + 4) ==
              std::optional<std::uint32_t>{network::protocol_ipv6} &&
          guest_memory.read32(guest_control + 8) ==
              std::optional<std::uint32_t>{network::ipv6_packet_info} &&
          guest_memory.read8(guest_control + 27) ==
              std::optional<std::uint8_t>{1} &&
          guest_memory.read32(guest_control + 28) ==
              std::optional<std::uint32_t>{1} &&
          guest_memory.read32(guest_control + 32) ==
              std::optional<std::uint32_t>{16} &&
          guest_memory.read32(guest_control + 36) ==
              std::optional<std::uint32_t>{network::protocol_ipv6} &&
          guest_memory.read32(guest_control + 40) ==
              std::optional<std::uint32_t>{network::ipv6_hop_limit} &&
          guest_memory.read32(guest_message +
                              socket::arm32_message::flags_offset) ==
              std::optional<std::uint32_t>{0},
      "guest IPv6 recvmsg did not return ARM32 pktinfo/hop-limit controls");
}

void socket_option_syscall_test() {
  AddressSpace memory;
  constexpr std::uint32_t value_address = 0x3a000;
  constexpr std::uint32_t size_address = value_address + 8;
  require(memory.map(value_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "socket option memory map failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  cpu.registers()[0] = 1; // AF_UNIX
  cpu.registers()[1] = 2; // SOCK_DGRAM
  cpu.registers()[2] = 0;
  cpu.registers()[12] = 97;
  kernel.dispatch(cpu, 0x80);
  const auto fd = cpu.registers()[0];

  require(memory.write32(value_address, 53'256),
          "socket option value write failed");
  cpu.registers()[0] = fd;
  cpu.registers()[1] = 0xffff; // SOL_SOCKET
  cpu.registers()[2] = 0x1002; // SO_RCVBUF
  cpu.registers()[3] = value_address;
  cpu.registers()[4] = 4;
  cpu.registers()[12] = 105;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "setsockopt failed");

  require(memory.write32(value_address, 0),
          "socket option output clear failed");
  require(memory.write32(size_address, 4), "socket option size write failed");
  cpu.registers()[0] = fd;
  cpu.registers()[1] = 0xffff;
  cpu.registers()[2] = 0x1002;
  cpu.registers()[3] = value_address;
  cpu.registers()[4] = size_address;
  cpu.registers()[12] = 118;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "getsockopt failed");
  require(memory.read32(value_address) == std::optional<std::uint32_t>{53'256},
          "getsockopt returned the wrong value");
  require(memory.read32(size_address) == std::optional<std::uint32_t>{4},
          "getsockopt returned the wrong size");

  cpu.registers()[0] = fd;
  cpu.registers()[1] = value_address;
  cpu.registers()[2] = size_address;
  cpu.registers()[12] = 31;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 57,
          "getpeername on an unconnected socket did not return ENOTCONN");

  require(memory.write32(size_address, 16),
          "getsockname capacity write failed");
  cpu.registers()[0] = fd;
  cpu.registers()[1] = value_address;
  cpu.registers()[2] = size_address;
  cpu.registers()[12] = 32;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "getsockname failed");
  require(memory.read8(value_address + 1) == std::optional<std::uint8_t>{1},
          "getsockname returned the wrong address family");
}

void blocking_socket_read_test() {
  AddressSpace memory;
  constexpr std::uint32_t result_address = 0x3d000;
  constexpr std::uint32_t input_address = result_address + 0x20;
  constexpr std::uint32_t output_address = result_address + 0x40;
  require(memory.map(result_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "blocking socket memory map failed");
  const std::array<std::byte, 2> payload{std::byte{'o'}, std::byte{'k'}};
  require(memory.copy_in(input_address, payload),
          "blocking socket payload copy failed");
  Dynarmic::ExclusiveMonitor monitor{2};
  Cpu reader{0, memory, monitor};
  Cpu writer{1, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  writer.registers()[0] = darwin::socket::local;
  writer.registers()[1] = darwin::socket::stream;
  writer.registers()[2] = 0;
  writer.registers()[3] = result_address;
  writer.registers()[12] = darwin::syscall::socket_pair;
  kernel.dispatch(writer, 0x80);
  require(writer.registers()[0] == 0, "socketpair creation failed");
  const auto first = memory.read32(result_address).value_or(0);
  const auto second = memory.read32(result_address + 4).value_or(0);
  require(first >= 3 && second >= 3, "socketpair returned invalid descriptors");

  reader.registers()[0] = first;
  reader.registers()[1] = output_address;
  reader.registers()[2] = static_cast<std::uint32_t>(payload.size());
  reader.registers()[12] = darwin::syscall::read;
  kernel.dispatch(reader, 0x80);
  require(kernel.wait_reason(0) == "read(fd=" + std::to_string(first) + ")",
          "empty stream read did not block");

  writer.registers()[0] = second;
  writer.registers()[1] = input_address;
  writer.registers()[2] = static_cast<std::uint32_t>(payload.size());
  writer.registers()[12] = darwin::syscall::write;
  kernel.dispatch(writer, 0x80);
  require(writer.registers()[0] == payload.size(),
          "socket stream write failed");

  constexpr std::uint32_t option_value_address = result_address + 0x80;
  constexpr std::uint32_t option_size_address = result_address + 0x84;
  require(memory.write32(option_size_address, sizeof(std::uint32_t)),
          "SO_NREAD size setup failed");
  writer.registers()[0] = first;
  writer.registers()[1] = darwin::socket::option_level;
  writer.registers()[2] = darwin::socket::option_pending_bytes;
  writer.registers()[3] = option_value_address;
  writer.registers()[4] = option_size_address;
  writer.registers()[12] = 118; // getsockopt
  kernel.dispatch(writer, 0x80);
  require(writer.registers()[0] == 0 &&
              memory.read32(option_value_address) ==
                  std::optional<std::uint32_t>{payload.size()},
          "SO_NREAD did not report queued local-stream bytes");
  writer.registers()[0] = first;
  writer.registers()[1] = darwin::socket::ioctl_pending_bytes;
  writer.registers()[2] = option_value_address;
  writer.registers()[12] = 54; // ioctl(FIONREAD)
  kernel.dispatch(writer, 0x80);
  require(writer.registers()[0] == 0 &&
              memory.read32(option_value_address) ==
                  std::optional<std::uint32_t>{payload.size()},
          "FIONREAD did not report queued local-stream bytes");

  require(kernel.deliver_pending_io(reader),
          "socket data did not wake the blocked reader");
  require(reader.registers()[0] == payload.size(),
          "blocked socket read returned the wrong byte count");
  require(memory.read_bytes(output_address, payload.size()) ==
              std::optional<std::vector<std::byte>>{
                  std::vector<std::byte>{payload.begin(), payload.end()}},
          "blocked socket read returned the wrong payload");

  writer.registers()[0] = second;
  writer.registers()[12] = 41; // dup
  kernel.dispatch(writer, 0x80);
  const auto duplicated_writer = writer.registers()[0];
  require(duplicated_writer >= 3 && duplicated_writer != second,
          "socket endpoint dup failed");

  writer.registers()[0] = second;
  writer.registers()[12] = 6; // close
  kernel.dispatch(writer, 0x80);
  require(writer.registers()[0] == 0, "original socket endpoint close failed");

  reader.registers()[0] = first;
  reader.registers()[1] = output_address;
  reader.registers()[2] = 1;
  reader.registers()[12] = darwin::syscall::read;
  kernel.dispatch(reader, 0x80);
  require(kernel.wait_reason(0) == "read(fd=" + std::to_string(first) + ")",
          "closing one dup reference incorrectly produced EOF");

  writer.registers()[0] = duplicated_writer;
  writer.registers()[12] = 6;
  kernel.dispatch(writer, 0x80);
  require(writer.registers()[0] == 0, "last socket endpoint close failed");
  require(kernel.deliver_pending_io(reader),
          "last endpoint close did not wake the blocked peer");
  require(reader.registers()[0] == 0,
          "drained stream did not return EOF after final close");
}

void nonblocking_socket_receive_test() {
  AddressSpace memory;
  constexpr std::uint32_t base = 0x3d400;
  constexpr std::uint32_t pair_address = base;
  constexpr std::uint32_t message_address = base + 0x40;
  constexpr std::uint32_t iovec_address = base + 0x80;
  constexpr std::uint32_t output_address = base + 0xc0;
  constexpr std::uint32_t input_address = base + 0x100;
  const std::array<std::byte, 2> payload{std::byte{'n'}, std::byte{'b'}};
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write) &&
              memory.copy_in(input_address, payload),
          "nonblocking socket memory setup failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  cpu.registers()[0] = darwin::socket::local;
  cpu.registers()[1] = darwin::socket::stream;
  cpu.registers()[2] = 0;
  cpu.registers()[3] = pair_address;
  cpu.registers()[12] = darwin::syscall::socket_pair;
  kernel.dispatch(cpu, 0x80);
  const auto receiver = memory.read32(pair_address).value_or(0);
  const auto sender = memory.read32(pair_address + 4).value_or(0);
  require(cpu.registers()[0] == 0 && receiver >= 3 && sender >= 3,
          "nonblocking socketpair creation failed");

  cpu.registers()[0] = receiver;
  cpu.registers()[1] = darwin::fcntl_command::set_status_flags;
  cpu.registers()[2] =
      darwin::open_flag::read_write | darwin::open_flag::non_block;
  cpu.registers()[12] = darwin::syscall::fcntl;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "nonblocking socket F_SETFL failed");

  cpu.registers()[0] = receiver;
  cpu.registers()[1] = output_address;
  cpu.registers()[2] = payload.size();
  cpu.registers()[12] = darwin::syscall::read;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == darwin::error::would_block &&
              (cpu.cpsr() & (1U << 29U)) != 0 &&
              kernel.wait_reason(0) == "none",
          "empty nonblocking read suspended instead of returning EWOULDBLOCK");

  require(
      memory.write32(
          message_address + darwin::socket::arm32_message::name_offset, 0) &&
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
              0) &&
          memory.write32(message_address +
                             darwin::socket::arm32_message::flags_offset,
                         0) &&
          memory.write32(iovec_address +
                             darwin::socket::arm32_iovec::base_offset,
                         output_address) &&
          memory.write32(iovec_address +
                             darwin::socket::arm32_iovec::length_offset,
                         payload.size()),
      "nonblocking recvmsg structure setup failed");
  cpu.registers()[0] = receiver;
  cpu.registers()[1] = message_address;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = darwin::syscall::receive_message;
  kernel.dispatch(cpu, 0x80);
  require(
      cpu.registers()[0] == darwin::error::would_block &&
          (cpu.cpsr() & (1U << 29U)) != 0 && kernel.wait_reason(0) == "none",
      "empty nonblocking recvmsg suspended instead of returning EWOULDBLOCK");

  cpu.registers()[0] = sender;
  cpu.registers()[1] = input_address;
  cpu.registers()[2] = payload.size();
  cpu.registers()[12] = darwin::syscall::write;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == payload.size(),
          "nonblocking receive test write failed");
  cpu.registers()[0] = receiver;
  cpu.registers()[1] = message_address;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = darwin::syscall::receive_message;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == payload.size() &&
              (cpu.cpsr() & (1U << 29U)) == 0 &&
              memory.read_bytes(output_address, payload.size()) ==
                  std::optional<std::vector<std::byte>>{
                      std::vector<std::byte>{payload.begin(), payload.end()}},
          "nonblocking recvmsg did not consume available stream data");
}

} // namespace

void run_host_socket_tests() {
  host_network_loopback_test();
  mdns_host_socket_options_test();
  socket_option_syscall_test();
  blocking_socket_read_test();
  nonblocking_socket_receive_test();
}

} // namespace ilemu::test::network_suite
