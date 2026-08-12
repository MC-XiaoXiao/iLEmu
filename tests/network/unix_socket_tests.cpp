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

void cross_process_unix_listener_lifetime_test() {
  constexpr std::uint32_t base = 0x3d800;
  constexpr std::uint32_t sockaddr_address = base;
  constexpr std::uint32_t input_address = base + 0x100;
  constexpr std::uint32_t output_address = base + 0x180;
  constexpr std::string_view name{"/var/run/mdns-lifetime"};

  AddressSpace parent_memory;
  AddressSpace child_memory;
  require(
      parent_memory.map(base, AddressSpace::page_size,
                        MemoryPermission::Read | MemoryPermission::Write) &&
          child_memory.map(base, AddressSpace::page_size,
                           MemoryPermission::Read | MemoryPermission::Write),
      "cross-process Unix socket memory map failed");
  std::vector<std::byte> address(2U + name.size() + 1U, std::byte{0});
  address[0] = static_cast<std::byte>(address.size());
  address[1] = static_cast<std::byte>(darwin::socket::local);
  for (std::size_t index = 0; index < name.size(); ++index) {
    address[index + 2U] = static_cast<std::byte>(name[index]);
  }
  require(parent_memory.copy_in(sockaddr_address, address),
          "Unix listener sockaddr setup failed");

  Dynarmic::ExclusiveMonitor monitor{2};
  Cpu parent_cpu{0, parent_memory, monitor};
  Cpu child_cpu{1, child_memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel parent{parent_memory, output};
  CompatibilityKernel child{child_memory, output};

  parent_cpu.registers()[0] = darwin::socket::local;
  parent_cpu.registers()[1] = darwin::socket::stream;
  parent_cpu.registers()[2] = 0;
  parent_cpu.registers()[12] = darwin::syscall::socket;
  parent.dispatch(parent_cpu, 0x80);
  const auto listener_fd = parent_cpu.registers()[0];
  require(listener_fd >= 3, "Unix listener socket creation failed");

  parent_cpu.registers()[0] = listener_fd;
  parent_cpu.registers()[1] = sockaddr_address;
  parent_cpu.registers()[2] = static_cast<std::uint32_t>(address.size());
  parent_cpu.registers()[12] = darwin::syscall::bind;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == 0, "Unix listener bind failed");
  parent_cpu.registers()[0] = listener_fd;
  parent_cpu.registers()[1] = 8;
  parent_cpu.registers()[12] = darwin::syscall::listen;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == 0, "Unix listener listen failed");

  child.inherit_process_state(parent, 2);
  parent_cpu.registers()[0] = listener_fd;
  parent_cpu.registers()[12] = 6;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == 0,
          "parent listener descriptor close failed");

  parent_cpu.registers()[0] = darwin::socket::local;
  parent_cpu.registers()[1] = darwin::socket::stream;
  parent_cpu.registers()[2] = 0;
  parent_cpu.registers()[12] = darwin::syscall::socket;
  parent.dispatch(parent_cpu, 0x80);
  const auto client_fd = parent_cpu.registers()[0];
  parent_cpu.registers()[0] = client_fd;
  parent_cpu.registers()[1] = sockaddr_address;
  parent_cpu.registers()[2] = static_cast<std::uint32_t>(address.size());
  parent_cpu.registers()[12] = darwin::syscall::connect;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == 0,
          "fork-retained Unix listener rejected connect");

  child_cpu.registers()[0] = listener_fd;
  child_cpu.registers()[1] = 0;
  child_cpu.registers()[2] = 0;
  child_cpu.registers()[12] = darwin::syscall::accept;
  child.dispatch(child_cpu, 0x80);
  const auto accepted_fd = child_cpu.registers()[0];
  require(accepted_fd >= 3 && accepted_fd != listener_fd,
          "child failed to accept inherited Unix listener connection");

  const std::array<std::byte, 4> payload{std::byte{'m'}, std::byte{'D'},
                                         std::byte{'N'}, std::byte{'S'}};
  require(parent_memory.copy_in(input_address, payload),
          "cross-process Unix payload setup failed");
  parent_cpu.registers()[0] = client_fd;
  parent_cpu.registers()[1] = input_address;
  parent_cpu.registers()[2] = payload.size();
  parent_cpu.registers()[12] = darwin::syscall::write;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == payload.size(),
          "cross-process Unix stream write failed");

  child_cpu.registers()[0] = accepted_fd;
  child_cpu.registers()[1] = output_address;
  child_cpu.registers()[2] = payload.size();
  child_cpu.registers()[12] = darwin::syscall::read;
  child.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] == payload.size() &&
              child_memory.read_bytes(output_address, payload.size()) ==
                  std::optional<std::vector<std::byte>>{
                      std::vector<std::byte>{payload.begin(), payload.end()}},
          "cross-process Unix stream payload mismatch");

  child_cpu.registers()[0] = accepted_fd;
  child_cpu.registers()[12] = 6;
  child.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] == 0, "accepted endpoint close failed");

  parent_cpu.registers()[0] = client_fd;
  parent_cpu.registers()[1] = output_address;
  parent_cpu.registers()[2] = 1;
  parent_cpu.registers()[12] = darwin::syscall::read;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == 0,
          "cross-process peer close did not produce EOF");

  parent_cpu.registers()[0] = client_fd;
  parent_cpu.registers()[1] = input_address;
  parent_cpu.registers()[2] = 1;
  parent_cpu.registers()[12] = darwin::syscall::write;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == darwin::error::broken_pipe,
          "write after peer close did not return EPIPE");
}

void dnsservice_v120_control_socket_test() {
  constexpr std::uint32_t base = 0x4c000;
  constexpr std::uint32_t main_sockaddr = base;
  constexpr std::uint32_t control_sockaddr = base + 0x100;
  constexpr std::uint32_t request_input = base + 0x200;
  constexpr std::uint32_t daemon_header = base + 0x500;
  constexpr std::uint32_t daemon_body = base + 0x600;
  constexpr std::uint32_t status_input = base + 0x800;
  constexpr std::uint32_t status_output = base + 0x810;
  constexpr std::uint32_t unlink_path = base + 0x900;
  constexpr std::uint32_t reply_header_input = base + 0xa00;
  constexpr std::uint32_t reply_body_input = base + 0xa40;
  constexpr std::uint32_t client_reply_header = base + 0xb00;
  constexpr std::uint32_t client_reply_body = base + 0xb40;
  constexpr std::uint32_t getaddr_request_input = base + 0xc00;
  constexpr std::uint32_t getaddr_daemon_header = base + 0xc80;
  constexpr std::uint32_t getaddr_daemon_body = base + 0xcc0;
  constexpr std::uint32_t getaddr_status_input = base + 0xd40;
  constexpr std::uint32_t getaddr_status_output = base + 0xd50;
  constexpr std::uint32_t getaddr_reply_input = base + 0xd80;
  constexpr std::uint32_t getaddr_reply_output = base + 0xe80;
  constexpr std::string_view control_path{"/tmp/dnssd_clippath.42-a5b-123456"};

  AddressSpace daemon_memory;
  AddressSpace client_memory;
  require(
      daemon_memory.map(base, AddressSpace::page_size,
                        MemoryPermission::Read | MemoryPermission::Write) &&
          client_memory.map(base, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write),
      "DNSService v120 memory map failed");

  const auto make_sockaddr = [](std::string_view path) {
    std::vector<std::byte> address(2U + path.size() + 1U, std::byte{0});
    address[0] = static_cast<std::byte>(address.size());
    address[1] = static_cast<std::byte>(darwin::socket::local);
    for (std::size_t index = 0; index < path.size(); ++index) {
      address[index + 2U] = static_cast<std::byte>(path[index]);
    }
    return address;
  };
  const auto server_address = make_sockaddr(dnssd_ipc::server_path);
  const auto control_address = make_sockaddr(control_path);
  require(daemon_memory.copy_in(main_sockaddr, server_address) &&
              daemon_memory.copy_in(control_sockaddr, control_address) &&
              client_memory.copy_in(main_sockaddr, server_address) &&
              client_memory.copy_in(control_sockaddr, control_address),
          "DNSService v120 sockaddr setup failed");

  std::vector<std::byte> control_string(control_path.size() + 1U, std::byte{0});
  for (std::size_t index = 0; index < control_path.size(); ++index) {
    control_string[index] = static_cast<std::byte>(control_path[index]);
  }
  require(client_memory.copy_in(unlink_path, control_string),
          "DNSService v120 cleanup path setup failed");

  Dynarmic::ExclusiveMonitor monitor{2};
  Cpu daemon_cpu{0, daemon_memory, monitor};
  Cpu client_cpu{1, client_memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel daemon{daemon_memory, output};
  CompatibilityKernel client{client_memory, output};

  const auto create_socket = [](CompatibilityKernel &kernel, Cpu &cpu) {
    cpu.registers()[0] = darwin::socket::local;
    cpu.registers()[1] = darwin::socket::stream;
    cpu.registers()[2] = 0;
    cpu.registers()[12] = darwin::syscall::socket;
    kernel.dispatch(cpu, 0x80);
    return cpu.registers()[0];
  };
  const auto bind_and_listen = [](CompatibilityKernel &kernel, Cpu &cpu,
                                  std::uint32_t fd, std::uint32_t address,
                                  std::size_t address_size) {
    cpu.registers()[0] = fd;
    cpu.registers()[1] = address;
    cpu.registers()[2] = static_cast<std::uint32_t>(address_size);
    cpu.registers()[12] = darwin::syscall::bind;
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == 0, "DNSService v120 listener bind failed");
    cpu.registers()[0] = fd;
    cpu.registers()[1] = 1;
    cpu.registers()[12] = darwin::syscall::listen;
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == 0, "DNSService v120 listener listen failed");
  };

  const auto daemon_listener = create_socket(daemon, daemon_cpu);
  require(daemon_listener >= 3, "mDNSResponder listener creation failed");
  bind_and_listen(daemon, daemon_cpu, daemon_listener, main_sockaddr,
                  server_address.size());

  // A real client is a separate task. Fork-style inheritance supplies the
  // shared kernel namespace; close the inherited launchd/daemon listener in
  // the client before creating its private control endpoint.
  client.inherit_process_state(daemon, 42);
  client_cpu.registers()[0] = daemon_listener;
  client_cpu.registers()[12] = darwin::syscall::close;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0,
          "DNSService client retained daemon listener fd");

  const auto control_listener = create_socket(client, client_cpu);
  bind_and_listen(client, client_cpu, control_listener, control_sockaddr,
                  control_address.size());
  client_cpu.registers()[0] = control_listener;
  client_cpu.registers()[1] = control_sockaddr;
  client_cpu.registers()[2] = control_address.size();
  client_cpu.registers()[12] = darwin::syscall::bind;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == darwin::error::invalid_argument,
          "AF_UNIX socket accepted a second bind");

  const auto primary_client = create_socket(client, client_cpu);
  client_cpu.registers()[0] = primary_client;
  client_cpu.registers()[1] = main_sockaddr;
  client_cpu.registers()[2] = server_address.size();
  client_cpu.registers()[12] = darwin::syscall::connect;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0,
          "DNSService primary connection failed");

  daemon_cpu.registers()[0] = daemon_listener;
  daemon_cpu.registers()[1] = 0;
  daemon_cpu.registers()[2] = 0;
  daemon_cpu.registers()[12] = darwin::syscall::accept;
  daemon.dispatch(daemon_cpu, 0x80);
  const auto primary_daemon = daemon_cpu.registers()[0];
  require(primary_daemon >= 3 && primary_daemon != daemon_listener,
          "mDNSResponder did not accept primary client");

  std::vector<std::byte> body = control_string;
  body.insert(body.end(), {std::byte{0x12}, std::byte{0x34}, std::byte{0x56},
                           std::byte{0x78}});
  const auto header = dnssd_ipc::encode_header(dnssd_ipc::Header{
      static_cast<std::uint32_t>(body.size()),
      0,
      dnssd_ipc::RequestOperation::RegisterRecord,
      {0x1122'3344U, 0x5566'7788U},
      7,
  });
  std::vector<std::byte> request{header.begin(), header.end()};
  request.insert(request.end(), body.begin(), body.end());
  require(client_memory.copy_in(request_input, request),
          "DNSService v120 request setup failed");

  // dnssd_clientstub's my_write/my_read loops permit partial stream I/O.
  // Force the 28-byte header across two writes and two daemon reads.
  constexpr std::uint32_t first_header_fragment = 11;
  client_cpu.registers()[0] = primary_client;
  client_cpu.registers()[1] = request_input;
  client_cpu.registers()[2] = first_header_fragment;
  client_cpu.registers()[3] = 0;
  client_cpu.registers()[4] = 0;
  client_cpu.registers()[5] = 0;
  client_cpu.registers()[12] = darwin::syscall::send_to;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == first_header_fragment,
          "DNSService first header fragment write failed");

  daemon_cpu.registers()[0] = primary_daemon;
  daemon_cpu.registers()[1] = daemon_header;
  daemon_cpu.registers()[2] = dnssd_ipc::header_size;
  daemon_cpu.registers()[3] = 0;
  daemon_cpu.registers()[4] = 0;
  daemon_cpu.registers()[5] = 0;
  daemon_cpu.registers()[12] = darwin::syscall::receive_from;
  daemon.dispatch(daemon_cpu, 0x80);
  require(daemon_cpu.registers()[0] == first_header_fragment,
          "DNSService partial header read was not preserved");

  client_cpu.registers()[0] = primary_client;
  client_cpu.registers()[1] = request_input + first_header_fragment;
  client_cpu.registers()[2] =
      static_cast<std::uint32_t>(request.size()) - first_header_fragment;
  client_cpu.registers()[3] = 0;
  client_cpu.registers()[4] = 0;
  client_cpu.registers()[5] = 0;
  client_cpu.registers()[12] = darwin::syscall::send_to;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == request.size() - first_header_fragment,
          "DNSService remaining request write failed");

  daemon_cpu.registers()[0] = primary_daemon;
  daemon_cpu.registers()[1] = daemon_header + first_header_fragment;
  daemon_cpu.registers()[2] = dnssd_ipc::header_size - first_header_fragment;
  daemon_cpu.registers()[3] = 0;
  daemon_cpu.registers()[4] = 0;
  daemon_cpu.registers()[5] = 0;
  daemon_cpu.registers()[12] = darwin::syscall::receive_from;
  daemon.dispatch(daemon_cpu, 0x80);
  require(daemon_cpu.registers()[0] ==
              dnssd_ipc::header_size - first_header_fragment,
          "DNSService remaining header read failed");
  daemon_cpu.registers()[0] = primary_daemon;
  daemon_cpu.registers()[1] = daemon_body;
  daemon_cpu.registers()[2] = body.size();
  daemon_cpu.registers()[3] = 0;
  daemon_cpu.registers()[4] = 0;
  daemon_cpu.registers()[5] = 0;
  daemon_cpu.registers()[12] = darwin::syscall::receive_from;
  daemon.dispatch(daemon_cpu, 0x80);
  require(daemon_cpu.registers()[0] == body.size(),
          "DNSService request body read failed");

  const auto received_header =
      daemon_memory.read_bytes(daemon_header, dnssd_ipc::header_size);
  const auto received_body = daemon_memory.read_bytes(daemon_body, body.size());
  require(received_header && received_body &&
              dnssd_ipc::decode_u32(received_header->data()) ==
                  dnssd_ipc::version &&
              dnssd_ipc::decode_u32(received_header->data() + 4) ==
                  body.size() &&
              dnssd_ipc::decode_u32(received_header->data() + 8) == 0 &&
              dnssd_ipc::decode_u32(received_header->data() + 12) ==
                  static_cast<std::uint32_t>(
                      dnssd_ipc::RequestOperation::RegisterRecord) &&
              dnssd_ipc::decode_arm32_u32(received_header->data() + 16) ==
                  0x1122'3344U &&
              dnssd_ipc::decode_arm32_u32(received_header->data() + 20) ==
                  0x5566'7788U &&
              dnssd_ipc::decode_u32(received_header->data() + 24) == 7 &&
              *received_body == body,
          "DNSService v120 network-order framing mismatch");

  // Normal asynchronous results travel in the opposite direction over the
  // primary stream. DNSServiceProcessResult may block before the daemon's
  // callback formats the result, so exercise both scheduler wakeups rather
  // than relying on pre-buffered bytes.
  const auto reply_prefix =
      dnssd_ipc::encode_reply_prefix(dnssd_ipc::ReplyPrefix{0, 2, 0});
  const auto reply_header = dnssd_ipc::encode_header(dnssd_ipc::ReplyHeader{
      static_cast<std::uint32_t>(reply_prefix.size()),
      0,
      dnssd_ipc::ReplyOperation::RegisterRecord,
      {0x1122'3344U, 0x5566'7788U},
      0,
  });
  require(daemon_memory.copy_in(reply_header_input, reply_header) &&
              daemon_memory.copy_in(reply_body_input, reply_prefix),
          "DNSService asynchronous reply setup failed");

  const auto begin_blocking_receive = [&](std::uint32_t address,
                                          std::uint32_t size) {
    client_cpu.registers()[0] = primary_client;
    client_cpu.registers()[1] = address;
    client_cpu.registers()[2] = size;
    client_cpu.registers()[3] = 0;
    client_cpu.registers()[4] = 0;
    client_cpu.registers()[5] = 0;
    client_cpu.registers()[12] = darwin::syscall::receive_from;
    client.dispatch(client_cpu, 0x80);
    require(client.wait_reason(client_cpu.processor_id()) ==
                "read(fd=" + std::to_string(primary_client) + ")",
            "DNSService asynchronous receive did not block");
  };
  const auto daemon_send = [&](std::uint32_t address, std::uint32_t size) {
    daemon_cpu.registers()[0] = primary_daemon;
    daemon_cpu.registers()[1] = address;
    daemon_cpu.registers()[2] = size;
    daemon_cpu.registers()[3] = 0;
    daemon_cpu.registers()[4] = 0;
    daemon_cpu.registers()[5] = 0;
    daemon_cpu.registers()[12] = darwin::syscall::send_to;
    daemon.dispatch(daemon_cpu, 0x80);
    require(daemon_cpu.registers()[0] == size,
            "mDNSResponder asynchronous reply write failed");
  };

  begin_blocking_receive(client_reply_header,
                         static_cast<std::uint32_t>(reply_header.size()));
  daemon_send(reply_header_input,
              static_cast<std::uint32_t>(reply_header.size()));
  require(client.deliver_pending_io(client_cpu) &&
              client_cpu.registers()[0] == reply_header.size(),
          "DNSService reply header did not wake the client");

  begin_blocking_receive(client_reply_body,
                         static_cast<std::uint32_t>(reply_prefix.size()));
  daemon_send(reply_body_input,
              static_cast<std::uint32_t>(reply_prefix.size()));
  require(client.deliver_pending_io(client_cpu) &&
              client_cpu.registers()[0] == reply_prefix.size(),
          "DNSService reply body did not wake the client");

  const auto received_reply_header =
      client_memory.read_bytes(client_reply_header, reply_header.size());
  const auto received_reply_body =
      client_memory.read_bytes(client_reply_body, reply_prefix.size());
  require(received_reply_header && received_reply_body &&
              dnssd_ipc::decode_u32(received_reply_header->data()) ==
                  dnssd_ipc::version &&
              dnssd_ipc::decode_u32(received_reply_header->data() + 4) ==
                  reply_prefix.size() &&
              dnssd_ipc::decode_u32(received_reply_header->data() + 12) ==
                  static_cast<std::uint32_t>(
                      dnssd_ipc::ReplyOperation::RegisterRecord) &&
              dnssd_ipc::decode_arm32_u32(received_reply_header->data() + 16) ==
                  0x1122'3344U &&
              dnssd_ipc::decode_arm32_u32(received_reply_header->data() + 20) ==
                  0x5566'7788U &&
              dnssd_ipc::decode_u32(received_reply_body->data()) == 0 &&
              dnssd_ipc::decode_u32(received_reply_body->data() + 4) == 2 &&
              dnssd_ipc::decode_u32(received_reply_body->data() + 8) == 0,
          "DNSService asynchronous reply framing mismatch");

  // The target's private v120 extension uses operation 14/71. Its request
  // body is three network-order words followed by a C hostname; each result
  // carries that hostname, a DNS type, a length-prefixed address, and TTL.
  // Drive this over the same two process endpoints used above so the codec,
  // synchronous status, stream framing, and scheduler wakeups stay coupled.
  constexpr std::string_view getaddr_hostname{"iphone.local"};
  const auto getaddr_body = dnssd_ipc::encode_get_address_info_request(
      dnssd_ipc::GetAddressInfoRequest{
          0,
          2,
          dnssd_ipc::get_address_info_protocol_ipv4 |
              dnssd_ipc::get_address_info_protocol_ipv6,
          getaddr_hostname,
      });
  require(getaddr_body.has_value(),
          "DNSService GetAddrInfo request encoding failed");
  const auto getaddr_header = dnssd_ipc::encode_header(dnssd_ipc::Header{
      static_cast<std::uint32_t>(getaddr_body->size()),
      dnssd_ipc::flag_reuse_socket,
      dnssd_ipc::RequestOperation::GetAddressInfo,
      {0x1020'3040U, 0},
      0,
  });
  std::vector<std::byte> getaddr_request{getaddr_header.begin(),
                                         getaddr_header.end()};
  getaddr_request.insert(getaddr_request.end(), getaddr_body->begin(),
                         getaddr_body->end());
  require(client_memory.copy_in(getaddr_request_input, getaddr_request),
          "DNSService GetAddrInfo request setup failed");

  client_cpu.registers()[0] = primary_client;
  client_cpu.registers()[1] = getaddr_request_input;
  client_cpu.registers()[2] = getaddr_request.size();
  client_cpu.registers()[3] = 0;
  client_cpu.registers()[4] = 0;
  client_cpu.registers()[5] = 0;
  client_cpu.registers()[12] = darwin::syscall::send_to;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == getaddr_request.size(),
          "DNSService GetAddrInfo request write failed");

  daemon_cpu.registers()[0] = primary_daemon;
  daemon_cpu.registers()[1] = getaddr_daemon_header;
  daemon_cpu.registers()[2] = getaddr_header.size();
  daemon_cpu.registers()[3] = 0;
  daemon_cpu.registers()[4] = 0;
  daemon_cpu.registers()[5] = 0;
  daemon_cpu.registers()[12] = darwin::syscall::receive_from;
  daemon.dispatch(daemon_cpu, 0x80);
  require(daemon_cpu.registers()[0] == getaddr_header.size(),
          "mDNSResponder GetAddrInfo header read failed");
  daemon_cpu.registers()[0] = primary_daemon;
  daemon_cpu.registers()[1] = getaddr_daemon_body;
  daemon_cpu.registers()[2] = getaddr_body->size();
  daemon_cpu.registers()[3] = 0;
  daemon_cpu.registers()[4] = 0;
  daemon_cpu.registers()[5] = 0;
  daemon_cpu.registers()[12] = darwin::syscall::receive_from;
  daemon.dispatch(daemon_cpu, 0x80);
  require(daemon_cpu.registers()[0] == getaddr_body->size(),
          "mDNSResponder GetAddrInfo body read failed");

  const auto received_getaddr_header =
      daemon_memory.read_bytes(getaddr_daemon_header, getaddr_header.size());
  const auto received_getaddr_body =
      daemon_memory.read_bytes(getaddr_daemon_body, getaddr_body->size());
  const auto decoded_getaddr =
      received_getaddr_body
          ? dnssd_ipc::decode_get_address_info_request(*received_getaddr_body)
          : std::nullopt;
  require(received_getaddr_header && decoded_getaddr &&
              dnssd_ipc::decode_u32(received_getaddr_header->data() + 8U) ==
                  dnssd_ipc::flag_reuse_socket &&
              dnssd_ipc::decode_u32(received_getaddr_header->data() + 12U) ==
                  static_cast<std::uint32_t>(
                      dnssd_ipc::RequestOperation::GetAddressInfo) &&
              decoded_getaddr->flags == 0 &&
              decoded_getaddr->interface_index == 2 &&
              decoded_getaddr->protocols ==
                  (dnssd_ipc::get_address_info_protocol_ipv4 |
                   dnssd_ipc::get_address_info_protocol_ipv6) &&
              decoded_getaddr->hostname == getaddr_hostname,
          "DNSService GetAddrInfo request framing mismatch");

  const auto getaddr_success = dnssd_ipc::encode_u32(0);
  require(daemon_memory.copy_in(getaddr_status_input, getaddr_success),
          "DNSService GetAddrInfo status setup failed");
  begin_blocking_receive(getaddr_status_output,
                         static_cast<std::uint32_t>(getaddr_success.size()));
  daemon_send(getaddr_status_input,
              static_cast<std::uint32_t>(getaddr_success.size()));
  require(client.deliver_pending_io(client_cpu) &&
              client_cpu.registers()[0] == getaddr_success.size(),
          "DNSService GetAddrInfo status did not wake the client");

  const std::array ipv4_address{std::byte{127}, std::byte{0}, std::byte{0},
                                std::byte{1}};
  const std::array ipv6_address{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
  const auto deliver_getaddr_reply = [&](dnssd_ipc::DnsRecordType type,
                                         std::span<const std::byte> address,
                                         std::uint32_t ttl) {
    const auto reply_body =
        dnssd_ipc::encode_get_address_info_reply(dnssd_ipc::GetAddressInfoReply{
            dnssd_ipc::ReplyPrefix{0, 2, 0},
            getaddr_hostname,
            type,
            address,
            ttl,
        });
    require(reply_body.has_value(),
            "DNSService GetAddrInfo reply encoding failed");
    const auto result_header = dnssd_ipc::encode_header(dnssd_ipc::ReplyHeader{
        static_cast<std::uint32_t>(reply_body->size()),
        0,
        dnssd_ipc::ReplyOperation::GetAddressInfo,
        {0x1020'3040U, 0},
        0,
    });
    require(daemon_memory.copy_in(getaddr_reply_input, result_header) &&
                daemon_memory.copy_in(
                    getaddr_reply_input + result_header.size(), *reply_body),
            "DNSService GetAddrInfo result setup failed");

    begin_blocking_receive(getaddr_reply_output,
                           static_cast<std::uint32_t>(result_header.size()));
    daemon_send(getaddr_reply_input,
                static_cast<std::uint32_t>(result_header.size()));
    require(client.deliver_pending_io(client_cpu) &&
                client_cpu.registers()[0] == result_header.size(),
            "DNSService GetAddrInfo result header wake failed");
    begin_blocking_receive(getaddr_reply_output + result_header.size(),
                           static_cast<std::uint32_t>(reply_body->size()));
    daemon_send(getaddr_reply_input + result_header.size(),
                static_cast<std::uint32_t>(reply_body->size()));
    require(client.deliver_pending_io(client_cpu) &&
                client_cpu.registers()[0] == reply_body->size(),
            "DNSService GetAddrInfo result body wake failed");

    const auto received_result_header =
        client_memory.read_bytes(getaddr_reply_output, result_header.size());
    const auto received_result_body = client_memory.read_bytes(
        getaddr_reply_output + result_header.size(), reply_body->size());
    const auto decoded_result =
        received_result_body
            ? dnssd_ipc::decode_get_address_info_reply(*received_result_body)
            : std::nullopt;
    require(received_result_header && decoded_result &&
                dnssd_ipc::decode_u32(received_result_header->data() + 12U) ==
                    static_cast<std::uint32_t>(
                        dnssd_ipc::ReplyOperation::GetAddressInfo) &&
                decoded_result->prefix.flags == 0 &&
                decoded_result->prefix.interface_index == 2 &&
                decoded_result->prefix.error == 0 &&
                decoded_result->hostname == getaddr_hostname &&
                decoded_result->record_type == type &&
                decoded_result->address.size() == address.size() &&
                std::equal(decoded_result->address.begin(),
                           decoded_result->address.end(), address.begin()) &&
                decoded_result->ttl == ttl,
            "DNSService GetAddrInfo result framing mismatch");
  };
  deliver_getaddr_reply(dnssd_ipc::DnsRecordType::A, ipv4_address, 60);
  deliver_getaddr_reply(dnssd_ipc::DnsRecordType::Aaaa, ipv6_address, 120);

  // deliver_request blocks here before the daemon has run the request
  // callback. The guest scheduler must suspend this client thread and wake
  // it only after mDNSResponder connects to the temporary control listener.
  client_cpu.registers()[0] = control_listener;
  client_cpu.registers()[1] = 0;
  client_cpu.registers()[2] = 0;
  client_cpu.registers()[12] = darwin::syscall::accept;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0 &&
              client.wait_reason(client_cpu.processor_id()) ==
                  "accept(fd=" + std::to_string(control_listener) + ")",
          "blocking DNSService accept did not suspend the client thread");

  const auto error_socket = create_socket(daemon, daemon_cpu);
  daemon_cpu.registers()[0] = error_socket;
  daemon_cpu.registers()[1] = control_sockaddr;
  daemon_cpu.registers()[2] = control_address.size();
  daemon_cpu.registers()[12] = darwin::syscall::connect;
  daemon.dispatch(daemon_cpu, 0x80);
  require(daemon_cpu.registers()[0] == 0,
          "mDNSResponder dedicated error-socket callback failed");
  daemon_cpu.registers()[0] = error_socket;
  daemon_cpu.registers()[1] = darwin::fcntl_command::set_status_flags;
  daemon_cpu.registers()[2] = darwin::open_flag::non_block;
  daemon_cpu.registers()[12] = darwin::syscall::fcntl;
  daemon.dispatch(daemon_cpu, 0x80);
  require(daemon_cpu.registers()[0] == 0,
          "mDNSResponder error socket could not become nonblocking");

  require(client.deliver_pending_io(client_cpu) &&
              client.wait_reason(client_cpu.processor_id()) == "none",
          "mDNSResponder callback did not wake blocking client accept");
  const auto accepted_error = client_cpu.registers()[0];
  require(accepted_error >= 3 && accepted_error != control_listener,
          "DNSService client did not accept dedicated error socket");

  const auto status = dnssd_ipc::encode_u32(
      static_cast<std::uint32_t>(dnssd_ipc::error_no_such_record));
  require(daemon_memory.copy_in(status_input, status),
          "DNSService synchronous status setup failed");
  daemon_cpu.registers()[0] = error_socket;
  daemon_cpu.registers()[1] = status_input;
  daemon_cpu.registers()[2] = status.size();
  daemon_cpu.registers()[3] = 0;
  daemon_cpu.registers()[4] = 0;
  daemon_cpu.registers()[5] = 0;
  daemon_cpu.registers()[12] = darwin::syscall::send_to;
  daemon.dispatch(daemon_cpu, 0x80);
  require(daemon_cpu.registers()[0] == status.size(),
          "mDNSResponder synchronous status write failed");

  for (std::uint32_t offset = 0; offset < status.size(); offset += 2) {
    client_cpu.registers()[0] = accepted_error;
    client_cpu.registers()[1] = status_output + offset;
    client_cpu.registers()[2] = 2;
    client_cpu.registers()[3] = 0;
    client_cpu.registers()[4] = 0;
    client_cpu.registers()[5] = 0;
    client_cpu.registers()[12] = darwin::syscall::receive_from;
    client.dispatch(client_cpu, 0x80);
    require(client_cpu.registers()[0] == 2,
            "DNSService fragmented status read failed");
  }
  const auto received_status = client_memory.read_bytes(status_output, 4);
  require(received_status &&
              std::bit_cast<std::int32_t>(dnssd_ipc::decode_u32(
                  received_status->data())) == dnssd_ipc::error_no_such_record,
          "DNSService synchronous status byte order changed");

  for (const auto fd : {accepted_error, control_listener}) {
    client_cpu.registers()[0] = fd;
    client_cpu.registers()[12] = darwin::syscall::close;
    client.dispatch(client_cpu, 0x80);
    require(client_cpu.registers()[0] == 0,
            "DNSService client control fd close failed");
  }
  daemon_cpu.registers()[0] = error_socket;
  daemon_cpu.registers()[12] = darwin::syscall::close;
  daemon.dispatch(daemon_cpu, 0x80);
  require(daemon_cpu.registers()[0] == 0,
          "mDNSResponder error fd close failed");

  // Closing a bound socket must leave its pathname occupied. The client's
  // cleanup unlink removes it and permits the next unique control listener.
  const auto rebound = create_socket(client, client_cpu);
  client_cpu.registers()[0] = rebound;
  client_cpu.registers()[1] = control_sockaddr;
  client_cpu.registers()[2] = control_address.size();
  client_cpu.registers()[12] = darwin::syscall::bind;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == darwin::error::address_in_use,
          "AF_UNIX pathname disappeared on close without unlink");

  client_cpu.registers()[0] = unlink_path;
  client_cpu.registers()[12] = darwin::syscall::unlink;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0,
          "DNSService control pathname unlink failed");
  client_cpu.registers()[0] = rebound;
  client_cpu.registers()[1] = control_sockaddr;
  client_cpu.registers()[2] = control_address.size();
  client_cpu.registers()[12] = darwin::syscall::bind;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0,
          "DNSService control pathname was not reusable after unlink");

  client_cpu.registers()[0] = unlink_path;
  client_cpu.registers()[12] = darwin::syscall::unlink;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0,
          "bound DNSService pathname unlink failed");
  client_cpu.registers()[0] = rebound;
  client_cpu.registers()[1] = 1;
  client_cpu.registers()[12] = darwin::syscall::listen;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0,
          "unlinked AF_UNIX socket could not enter listen state");

  const auto unreachable = create_socket(client, client_cpu);
  client_cpu.registers()[0] = unreachable;
  client_cpu.registers()[1] = control_sockaddr;
  client_cpu.registers()[2] = control_address.size();
  client_cpu.registers()[12] = darwin::syscall::connect;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == darwin::error::connection_refused,
          "listen resurrected an AF_UNIX pathname unlinked after bind");

  const auto replacement = create_socket(client, client_cpu);
  bind_and_listen(client, client_cpu, replacement, control_sockaddr,
                  control_address.size());
  const auto replacement_client = create_socket(client, client_cpu);
  client_cpu.registers()[0] = replacement_client;
  client_cpu.registers()[1] = control_sockaddr;
  client_cpu.registers()[2] = control_address.size();
  client_cpu.registers()[12] = darwin::syscall::connect;
  client.dispatch(client_cpu, 0x80);
  require(
      client_cpu.registers()[0] == 0,
      "unlinked AF_UNIX pathname could not be rebound while old socket lived");
}

void unix_stream_shutdown_test() {
  AddressSpace memory;
  constexpr std::uint32_t base = 0x3dc00;
  constexpr std::uint32_t pair_address = base;
  constexpr std::uint32_t input_address = base + 0x40;
  constexpr std::uint32_t output_address = base + 0x80;
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "Unix shutdown memory map failed");
  const std::array<std::byte, 2> payload{std::byte{'e'}, std::byte{'o'}};
  require(memory.copy_in(input_address, payload),
          "Unix shutdown payload setup failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  const auto create_pair = [&]() {
    cpu.registers()[0] = darwin::socket::local;
    cpu.registers()[1] = darwin::socket::stream;
    cpu.registers()[2] = 0;
    cpu.registers()[3] = pair_address;
    cpu.registers()[12] = darwin::syscall::socket_pair;
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == 0, "shutdown socketpair creation failed");
    return std::pair{memory.read32(pair_address).value_or(0),
                     memory.read32(pair_address + 4).value_or(0)};
  };

  const auto [reader_fd, writer_fd] = create_pair();
  cpu.registers()[0] = writer_fd;
  cpu.registers()[1] = input_address;
  cpu.registers()[2] = payload.size();
  cpu.registers()[12] = darwin::syscall::write;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == payload.size(), "write before SHUT_WR failed");
  cpu.registers()[0] = writer_fd;
  cpu.registers()[1] = darwin::socket::shutdown_write;
  cpu.registers()[12] = darwin::syscall::shutdown;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "SHUT_WR failed");

  cpu.registers()[0] = reader_fd;
  cpu.registers()[1] = output_address;
  cpu.registers()[2] = payload.size();
  cpu.registers()[12] = darwin::syscall::read;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == payload.size(),
          "SHUT_WR discarded buffered stream data");
  cpu.registers()[0] = reader_fd;
  cpu.registers()[1] = output_address;
  cpu.registers()[2] = 1;
  cpu.registers()[12] = darwin::syscall::read;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0,
          "SHUT_WR did not produce EOF after buffered data");

  const auto [closed_reader, live_writer] = create_pair();
  cpu.registers()[0] = closed_reader;
  cpu.registers()[1] = darwin::socket::shutdown_read;
  cpu.registers()[12] = darwin::syscall::shutdown;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "SHUT_RD failed");
  cpu.registers()[0] = live_writer;
  cpu.registers()[1] = input_address;
  cpu.registers()[2] = 1;
  cpu.registers()[12] = darwin::syscall::write;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == darwin::error::broken_pipe,
          "peer SHUT_RD did not reject stream write with EPIPE");
}

void scm_rights_listener_transfer_test() {
  AddressSpace memory;
  constexpr std::uint32_t base = 0x3f000;
  constexpr std::uint32_t sockaddr_address = base;
  constexpr std::uint32_t pair_address = base + 0x100;
  constexpr std::uint32_t send_message = base + 0x120;
  constexpr std::uint32_t send_iovec = base + 0x140;
  constexpr std::uint32_t send_control = base + 0x150;
  constexpr std::uint32_t send_byte = base + 0x170;
  constexpr std::uint32_t prefix_byte = base + 0x171;
  constexpr std::uint32_t receive_message = base + 0x180;
  constexpr std::uint32_t receive_iovec = base + 0x1a0;
  constexpr std::uint32_t receive_control = base + 0x1b0;
  constexpr std::uint32_t receive_byte = base + 0x1d0;
  constexpr std::string_view name{"/var/run/mdns-scm"};
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "SCM_RIGHTS listener test memory map failed");
  std::vector<std::byte> address(2U + name.size() + 1U, std::byte{0});
  address[0] = static_cast<std::byte>(address.size());
  address[1] = static_cast<std::byte>(darwin::socket::local);
  for (std::size_t index = 0; index < name.size(); ++index) {
    address[index + 2U] = static_cast<std::byte>(name[index]);
  }
  require(memory.copy_in(sockaddr_address, address) &&
              memory.write8(send_byte, static_cast<std::uint8_t>('L')) &&
              memory.write8(prefix_byte, static_cast<std::uint8_t>('P')),
          "SCM_RIGHTS listener wire setup failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  cpu.registers()[0] = darwin::socket::local;
  cpu.registers()[1] = darwin::socket::stream;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = darwin::syscall::socket;
  kernel.dispatch(cpu, 0x80);
  const auto listener_fd = cpu.registers()[0];
  cpu.registers()[0] = listener_fd;
  cpu.registers()[1] = sockaddr_address;
  cpu.registers()[2] = address.size();
  cpu.registers()[12] = darwin::syscall::bind;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "SCM listener bind failed");
  cpu.registers()[0] = listener_fd;
  cpu.registers()[1] = 8;
  cpu.registers()[12] = darwin::syscall::listen;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "SCM listener listen failed");
  cpu.registers()[0] = listener_fd;
  cpu.registers()[1] = darwin::fcntl_command::set_status_flags;
  cpu.registers()[2] =
      darwin::open_flag::read_write | darwin::open_flag::non_block;
  cpu.registers()[12] = darwin::syscall::fcntl;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "SCM listener nonblocking setup failed");

  cpu.registers()[0] = darwin::socket::local;
  cpu.registers()[1] = darwin::socket::stream;
  cpu.registers()[2] = 0;
  cpu.registers()[3] = pair_address;
  cpu.registers()[12] = darwin::syscall::socket_pair;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "SCM transport socketpair failed");
  const auto sender_fd = memory.read32(pair_address).value_or(0);
  const auto receiver_fd = memory.read32(pair_address + 4).value_or(0);

  // liblaunch can queue an ordinary sendmsg response immediately before a
  // response carrying listener fds. A recvmsg that coalesces both payloads
  // must deliver rights associated with the second byte, not an empty
  // ancillary record from the first message. DNSService v120's separate
  // pathname callback is covered by dnsservice_v120_control_socket_test.
  require(memory.write32(send_message + 0, 0) &&
              memory.write32(send_message + 4, 0) &&
              memory.write32(send_message + 8, send_iovec) &&
              memory.write32(send_message + 12, 1) &&
              memory.write32(send_message + 16, 0) &&
              memory.write32(send_message + 20, 0) &&
              memory.write32(send_message + 24, 0) &&
              memory.write32(send_iovec + 0, prefix_byte) &&
              memory.write32(send_iovec + 4, 1),
          "ordinary launchd sendmsg structure setup failed");
  cpu.registers()[0] = sender_fd;
  cpu.registers()[1] = send_message;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = darwin::syscall::send_message;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 1, "ordinary launchd sendmsg failed");

  require(memory.write32(send_message + 0, 0) &&
              memory.write32(send_message + 4, 0) &&
              memory.write32(send_message + 8, send_iovec) &&
              memory.write32(send_message + 12, 1) &&
              memory.write32(send_message + 16, send_control) &&
              memory.write32(send_message + 20, 16) &&
              memory.write32(send_message + 24, 0) &&
              memory.write32(send_iovec + 0, send_byte) &&
              memory.write32(send_iovec + 4, 1) &&
              memory.write32(send_control + 0, 16) &&
              memory.write32(send_control + 4, darwin::socket::option_level) &&
              memory.write32(send_control + 8, 1) && // SCM_RIGHTS
              memory.write32(send_control + 12, listener_fd),
          "SCM sendmsg structure setup failed");
  cpu.registers()[0] = sender_fd;
  cpu.registers()[1] = send_message;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = darwin::syscall::send_message;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 1, "listener SCM_RIGHTS sendmsg failed");

  cpu.registers()[0] = listener_fd;
  cpu.registers()[12] = 6;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "SCM sender listener close failed");

  cpu.registers()[0] = darwin::socket::local;
  cpu.registers()[1] = darwin::socket::stream;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = darwin::syscall::socket;
  kernel.dispatch(cpu, 0x80);
  const auto client_fd = cpu.registers()[0];
  cpu.registers()[0] = client_fd;
  cpu.registers()[1] = sockaddr_address;
  cpu.registers()[2] = address.size();
  cpu.registers()[12] = darwin::syscall::connect;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0,
          "in-flight SCM listener did not retain pathname registration");

  require(memory.write32(receive_message + 0, 0) &&
              memory.write32(receive_message + 4, 0) &&
              memory.write32(receive_message + 8, receive_iovec) &&
              memory.write32(receive_message + 12, 1) &&
              memory.write32(receive_message + 16, receive_control) &&
              memory.write32(receive_message + 20, 16) &&
              memory.write32(receive_message + 24, 0) &&
              memory.write32(receive_iovec + 0, receive_byte) &&
              memory.write32(receive_iovec + 4, 2),
          "SCM recvmsg structure setup failed");
  cpu.registers()[0] = receiver_fd;
  cpu.registers()[1] = receive_message;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = darwin::syscall::receive_message;
  kernel.dispatch(cpu, 0x80);
  require(
      cpu.registers()[0] == 2 &&
          memory.read8(receive_byte) == std::optional<std::uint8_t>{'P'} &&
          memory.read8(receive_byte + 1) == std::optional<std::uint8_t>{'L'} &&
          memory.read32(receive_control) == std::optional<std::uint32_t>{16} &&
          memory.read32(receive_control + 4) ==
              std::optional<std::uint32_t>{darwin::socket::option_level} &&
          memory.read32(receive_control + 8) == std::optional<std::uint32_t>{1},
      "SCM listener recvmsg control data mismatch");
  const auto transferred_listener =
      memory.read32(receive_control + 12).value_or(0);
  require(transferred_listener >= 3 && transferred_listener != listener_fd,
          "SCM_RIGHTS did not install a new listener fd");
  cpu.registers()[0] = transferred_listener;
  cpu.registers()[1] = darwin::fcntl_command::get_status_flags;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = darwin::syscall::fcntl;
  kernel.dispatch(cpu, 0x80);
  require((cpu.registers()[0] & darwin::open_flag::non_block) != 0,
          "SCM_RIGHTS did not carry listener file-status flags");

  cpu.registers()[0] = transferred_listener;
  cpu.registers()[1] = 0;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = darwin::syscall::accept;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] >= 3,
          "SCM-transferred listener could not accept queued client");
}

} // namespace

void run_unix_socket_tests() {
  cross_process_unix_listener_lifetime_test();
  dnsservice_v120_control_socket_test();
  unix_stream_shutdown_test();
  scm_rights_listener_transfer_test();
}

} // namespace ilemu::test::network_suite
