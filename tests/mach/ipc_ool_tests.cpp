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

namespace ilemu::test::mach_suite {
namespace {

using namespace ::ilemu;
using ::ilemu::test::require;

void cross_process_ool_mach_ipc_test() {
  constexpr std::uint32_t mach_send_message = 1;
  constexpr std::uint32_t mach_receive_message = 2;
  constexpr std::uint32_t mach_send_receive =
      mach_send_message | mach_receive_message;
  constexpr std::uint32_t configd_receive_options =
      mach_receive_message | (darwin::mig_wire::trailer_audit
                              << darwin::mig_wire::trailer_elements_shift);
  constexpr auto complex_copy_send_make_send_once =
      darwin::mig_wire::message_bits(
          darwin::mig_wire::disposition_copy_send,
          darwin::mig_wire::disposition_make_send_once, true);
  constexpr auto complex_move_send_once = darwin::mig_wire::message_bits(
      darwin::mig_wire::disposition_move_send_once, 0, true);
  constexpr auto make_send_port_descriptor =
      darwin::mig_wire::port_descriptor_metadata(
          darwin::mig_wire::disposition_make_send);
  constexpr auto move_send_port_descriptor =
      darwin::mig_wire::port_descriptor_metadata(
          darwin::mig_wire::disposition_move_send);
  constexpr std::uint32_t kernel_reply_name = 0x900U;
  AddressSpace server_memory;
  AddressSpace client_memory;
  constexpr std::uint32_t server_buffer = 0x41000;
  constexpr std::uint32_t client_buffer = 0x42000;
  constexpr std::uint32_t client_payload = 0x43000;
  constexpr std::uint32_t client_options = 0x44000;
  require(server_memory.map(server_buffer, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write),
          "OOL server buffer map failed");
  require(client_memory.map(client_buffer, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write),
          "OOL client buffer map failed");
  require(client_memory.map(client_payload, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write),
          "OOL client payload map failed");
  require(client_memory.map(client_options, AddressSpace::page_size * 2U,
                            MemoryPermission::Read | MemoryPermission::Write),
          "OOL client options map failed");
  const std::array<std::byte, 4> payload{std::byte{'n'}, std::byte{'a'},
                                         std::byte{'m'}, std::byte{'e'}};
  // Keep the first payload within one page and make the second span two.
  // A range-wide `mapped()` probe used to mistake the partially occupied
  // receive range for a free one, so the second mapping replaced the first.
  std::vector<std::byte> options(AddressSpace::page_size + 3U,
                                 std::byte{'x'});
  options.front() = std::byte{'<'};
  options[1] = std::byte{'/'};
  options.back() = std::byte{'>'};
  require(client_memory.copy_in(client_payload, payload) &&
              client_memory.copy_in(client_options, options),
          "OOL client configopen data copy failed");

  Dynarmic::ExclusiveMonitor server_monitor{1};
  Dynarmic::ExclusiveMonitor client_monitor{1};
  Cpu server_cpu{0, server_memory, server_monitor};
  Cpu client_cpu{0, client_memory, client_monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel server{server_memory, output};
  CompatibilityKernel client{client_memory, output};
  client.inherit_process_state(server, 2);

  client_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  client.dispatch(client_cpu, 0x80);
  const auto client_reply_port = client_cpu.registers()[0];
  require(client_reply_port != 0,
          "configopen reply receive right allocation failed");

  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = configd_receive_options;
  server_cpu.registers()[2] = 0;
  server_cpu.registers()[3] = 128;
  server_cpu.registers()[4] = server.process().bootstrap_port;
  server.dispatch(server_cpu, 0x80);

  require(
      client_memory.write32(client_buffer, complex_copy_send_make_send_once),
      "OOL message bits write failed");
  require(client_memory.write32(client_buffer + 4, 68),
          "OOL message size write failed");
  require(
      client_memory.write32(client_buffer + 8, server.process().bootstrap_port),
      "OOL message destination write failed");
  require(client_memory.write32(client_buffer + 12, client_reply_port),
          "OOL message reply write failed");
  require(client_memory.write32(
              client_buffer + 20,
              xnu792::mig::system_configuration::id(
                  xnu792::mig::system_configuration::Routine::configopen)),
          "OOL message id write failed");
  require(client_memory.write32(client_buffer + 24, 2),
          "OOL descriptor count write failed");
  require(client_memory.write32(client_buffer + 28, client_payload),
          "OOL descriptor address write failed");
  require(client_memory.write32(client_buffer + 32,
                                static_cast<std::uint32_t>(payload.size())),
          "OOL descriptor size write failed");
  constexpr auto deallocate_ool_descriptor =
      darwin::mig_wire::ool_descriptor_metadata(true);
  require(client_memory.write32(client_buffer + 36, deallocate_ool_descriptor),
          "OOL descriptor type write failed");
  require(
      client_memory.write32(client_buffer + 40, client_options) &&
          client_memory.write32(client_buffer + 44,
                                static_cast<std::uint32_t>(options.size())) &&
          client_memory.write32(client_buffer + 48, deallocate_ool_descriptor),
      "second configopen OOL descriptor write failed");
  require(
      client_memory.write32(client_buffer + 52, 0) &&
          client_memory.write32(client_buffer + 56, 1) &&
          client_memory.write32(client_buffer + 60,
                                static_cast<std::uint32_t>(payload.size())) &&
          client_memory.write32(client_buffer + 64,
                                static_cast<std::uint32_t>(options.size())),
      "configopen NDR/count words write failed");
  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = mach_send_message;
  client_cpu.registers()[2] = 68;
  client_cpu.registers()[3] = 0;
  client_cpu.registers()[4] = 0;
  constexpr std::uint32_t invalid_destination = 0xf0000000U;
  require(client_memory.write32(client_buffer + 8, invalid_destination),
          "failed OOL destination write failed");
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0x10000003U &&
              client_memory.mapped(client_payload) &&
              client_memory.mapped(client_options),
          "failed OOL send deallocated sender VM");
  require(
      client_memory.write32(client_buffer + 8, server.process().bootstrap_port),
      "restoring OOL destination failed");
  client_cpu.registers()[0] = client_buffer;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0, "OOL Mach send failed");
  require(!client_memory.mapped(client_payload) &&
              !client_memory.mapped(client_options),
          "successful OOL deallocate send retained sender VM");
  require(server.deliver_pending_mach(server_cpu),
          "OOL Mach receive did not wake server");
  const auto server_reply_name =
      server_memory.read32(server_buffer + 8).value_or(0);
  require(
      server_reply_name != 0 &&
          server_memory.read32(server_buffer + 12) ==
              std::optional<std::uint32_t>{server.process().bootstrap_port} &&
          server_memory.read32(server_buffer + 68) ==
              std::optional<std::uint32_t>{0} &&
          server_memory.read32(server_buffer + 72) ==
              std::optional<std::uint32_t>{
                  darwin::mig_wire::trailer_audit_size} &&
          server_memory.read32(server_buffer + 108) ==
              std::optional<std::uint32_t>{client.process().pid},
      "configopen reply right, destination, or audit trailer mismatch");
  const auto copied_address =
      server_memory.read32(server_buffer + 28).value_or(0);
  const auto copied_options =
      server_memory.read32(server_buffer + 40).value_or(0);
  require(copied_address != 0 && copied_address != client_payload,
          "OOL descriptor address was not copied out");
  require(copied_options != 0 && copied_options != client_options &&
              copied_options != copied_address,
          "second OOL descriptor address was not copied out");
  require(server_memory.read_bytes(copied_address, payload.size()) ==
              std::optional<std::vector<std::byte>>{
                  std::vector<std::byte>{payload.begin(), payload.end()}},
          "OOL payload bytes did not cross address spaces");
  require(server_memory.read_bytes(copied_options, options.size()) ==
                  std::optional<std::vector<std::byte>>{
                      std::vector<std::byte>{options.begin(), options.end()}} &&
              server_memory.read32(server_buffer + 60) ==
                  std::optional<std::uint32_t>{
                      static_cast<std::uint32_t>(payload.size())} &&
              server_memory.read32(server_buffer + 64) ==
                  std::optional<std::uint32_t>{
                      static_cast<std::uint32_t>(options.size())} &&
              stream.str().find("mig=config.configopen") != std::string::npos,
          "configopen OOL counts, options, or MIG trace label mismatch");

  // Match configd's configopen server path: allocate a receive right for the
  // new session, manufacture a same-name send right, and move that send
  // right to the client in the complex MIG reply.
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  server.dispatch(server_cpu, 0x80);
  const auto server_session = server_cpu.registers()[0];
  require(server_session != 0,
          "configopen session receive right allocation failed");

  const auto insert_right_id = xnu792::mig::mach_port::id(
      xnu792::mig::mach_port::Routine::mach_port_insert_right);
  require(
      server_memory.write32(server_buffer, complex_copy_send_make_send_once) &&
          server_memory.write32(server_buffer + 4, 52) &&
          server_memory.write32(server_buffer + 8,
                                server.process().task_port) &&
          server_memory.write32(server_buffer + 12, kernel_reply_name) &&
          server_memory.write32(server_buffer + 20, insert_right_id) &&
          server_memory.write32(server_buffer + 24, 1) &&
          server_memory.write32(server_buffer + 28, server_session) &&
          server_memory.write32(server_buffer + 32, 0) &&
          server_memory.write32(server_buffer + 36,
                                make_send_port_descriptor) &&
          server_memory.write32(server_buffer + 40, 0) &&
          server_memory.write32(server_buffer + 44, 1) &&
          server_memory.write32(server_buffer + 48, server_session),
      "configopen session send-right insertion setup failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = mach_send_receive;
  server_cpu.registers()[2] = 52;
  server_cpu.registers()[3] = 64;
  server_cpu.registers()[4] = kernel_reply_name;
  server.dispatch(server_cpu, 0x80);
  require(server_cpu.registers()[0] == 0 &&
              server_memory.read32(server_buffer + 32) ==
                  std::optional<std::uint32_t>{0},
          "configopen session send-right insertion failed");

  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = mach_receive_message;
  client_cpu.registers()[2] = 0;
  client_cpu.registers()[3] = 128;
  client_cpu.registers()[4] = client_reply_port;
  client.dispatch(client_cpu, 0x80);

  const auto configopen_reply_id =
      xnu792::mig::system_configuration::id(
          xnu792::mig::system_configuration::Routine::configopen) +
      100U;
  require(server_memory.write32(server_buffer, complex_move_send_once) &&
              server_memory.write32(server_buffer + 4, 52) &&
              server_memory.write32(server_buffer + 8, server_reply_name) &&
              server_memory.write32(server_buffer + 12, 0) &&
              server_memory.write32(server_buffer + 16, 0) &&
              server_memory.write32(server_buffer + 20, configopen_reply_id) &&
              server_memory.write32(server_buffer + 24, 1) &&
              server_memory.write32(server_buffer + 28, server_session) &&
              server_memory.write32(server_buffer + 32, 0) &&
              server_memory.write32(server_buffer + 36,
                                    move_send_port_descriptor) &&
              server_memory.write32(server_buffer + 40, 0) &&
              server_memory.write32(server_buffer + 44, 1) &&
              server_memory.write32(server_buffer + 48, 0),
          "configopen complex reply setup failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = mach_send_message;
  server_cpu.registers()[2] = 52;
  server_cpu.registers()[3] = 0;
  server_cpu.registers()[4] = 0;
  server.dispatch(server_cpu, 0x80);
  require(server_cpu.registers()[0] == 0,
          "configopen complex reply send failed");
  require(client.deliver_pending_mach(client_cpu),
          "configopen complex reply did not wake client");

  const auto client_session =
      client_memory.read32(client_buffer + 28).value_or(0);
  require(client_memory.read32(client_buffer + 4) ==
                  std::optional<std::uint32_t>{52} &&
              client_memory.read32(client_buffer + 20) ==
                  std::optional<std::uint32_t>{configopen_reply_id} &&
              client_memory.read32(client_buffer + 24) ==
                  std::optional<std::uint32_t>{1} &&
              client_memory.read32(client_buffer + 36) ==
                  std::optional<std::uint32_t>{move_send_port_descriptor} &&
              client_memory.read32(client_buffer + 48) ==
                  std::optional<std::uint32_t>{0} &&
              client_session != 0,
          "configopen complex reply layout or session copyout mismatch");

  const auto query_port_type = [&](CompatibilityKernel &kernel, Cpu &cpu,
                                   AddressSpace &memory, std::uint32_t buffer,
                                   std::uint32_t name) {
    const auto port_type_id = xnu792::mig::mach_port::id(
        xnu792::mig::mach_port::Routine::mach_port_type);
    require(memory.write32(buffer, 0x1513U) && memory.write32(buffer + 4, 36) &&
                memory.write32(buffer + 8, kernel.process().task_port) &&
                memory.write32(buffer + 12, kernel_reply_name) &&
                memory.write32(buffer + 20, port_type_id) &&
                memory.write32(buffer + 32, name),
            "configopen session port-type request setup failed");
    cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    cpu.registers()[0] = buffer;
    cpu.registers()[1] = mach_send_receive;
    cpu.registers()[2] = 36;
    cpu.registers()[3] = 64;
    cpu.registers()[4] = kernel_reply_name;
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == 0 &&
                memory.read32(buffer + 32) == std::optional<std::uint32_t>{0},
            "configopen session port-type query failed");
    return memory.read32(buffer + 36).value_or(0);
  };
  require(query_port_type(client, client_cpu, client_memory, client_buffer,
                          client_session) ==
              xnu792::ipc::type_mask(xnu792::ipc::Right::Send),
          "client did not receive the config session send right");
  require(query_port_type(server, server_cpu, server_memory, server_buffer,
                          server_session) ==
              xnu792::ipc::type_mask(xnu792::ipc::Right::Receive),
          "MOVE_SEND did not leave configd holding only the receive right");

  // Exercise the reverse OOL direction used by configget: the framework
  // supplies an XML key, then configd returns deallocated XML bytes plus
  // instance/status scalars in a complex reply.
  constexpr std::uint32_t client_key = 0x45000;
  constexpr std::uint32_t server_value = 0x46000;
  const std::array<std::byte, 5> key{std::byte{'S'}, std::byte{'t'},
                                     std::byte{'a'}, std::byte{'t'},
                                     std::byte{'e'}};
  const std::array<std::byte, 6> value{std::byte{'<'}, std::byte{'t'},
                                       std::byte{'r'}, std::byte{'u'},
                                       std::byte{'e'}, std::byte{'>'}};
  require(client_memory.map(client_key, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write) &&
              client_memory.copy_in(client_key, key),
          "configget key mapping failed");
  client_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  client.dispatch(client_cpu, 0x80);
  const auto configget_reply_port = client_cpu.registers()[0];
  require(configget_reply_port != 0, "configget reply port allocation failed");

  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = configd_receive_options;
  server_cpu.registers()[2] = 0;
  server_cpu.registers()[3] = 128;
  server_cpu.registers()[4] = server_session;
  server.dispatch(server_cpu, 0x80);

  constexpr auto virtual_copy_ool_descriptor =
      darwin::mig_wire::ool_descriptor_metadata(false);
  const auto configget_id = xnu792::mig::system_configuration::id(
      xnu792::mig::system_configuration::Routine::configget);
  require(
      client_memory.write32(client_buffer, complex_copy_send_make_send_once) &&
          client_memory.write32(client_buffer + 4, 52) &&
          client_memory.write32(client_buffer + 8, client_session) &&
          client_memory.write32(client_buffer + 12, configget_reply_port) &&
          client_memory.write32(client_buffer + 20, configget_id) &&
          client_memory.write32(client_buffer + 24, 1) &&
          client_memory.write32(client_buffer + 28, client_key) &&
          client_memory.write32(client_buffer + 32,
                                static_cast<std::uint32_t>(key.size())) &&
          client_memory.write32(client_buffer + 36,
                                virtual_copy_ool_descriptor) &&
          client_memory.write32(client_buffer + 40, 0) &&
          client_memory.write32(client_buffer + 44, 1) &&
          client_memory.write32(client_buffer + 48,
                                static_cast<std::uint32_t>(key.size())),
      "configget request setup failed");
  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = mach_send_message;
  client_cpu.registers()[2] = 52;
  client_cpu.registers()[3] = 0;
  client_cpu.registers()[4] = 0;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0 && client_memory.mapped(client_key),
          "configget request send or non-deallocate semantics failed");
  require(server.deliver_pending_mach(server_cpu),
          "configget request did not wake configd session");
  const auto configget_server_reply =
      server_memory.read32(server_buffer + 8).value_or(0);
  const auto copied_key = server_memory.read32(server_buffer + 28).value_or(0);
  require(configget_server_reply != 0 && copied_key != 0 &&
              server_memory.read_bytes(copied_key, key.size()) ==
                  std::optional<std::vector<std::byte>>{
                      std::vector<std::byte>{key.begin(), key.end()}} &&
              server_memory.read32(server_buffer + 48) ==
                  std::optional<std::uint32_t>{
                      static_cast<std::uint32_t>(key.size())},
          "configget key OOL copyin or count mismatch");

  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = mach_receive_message;
  client_cpu.registers()[2] = 0;
  client_cpu.registers()[3] = 128;
  client_cpu.registers()[4] = configget_reply_port;
  client.dispatch(client_cpu, 0x80);
  require(server_memory.map(server_value, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write) &&
              server_memory.copy_in(server_value, value),
          "configget server value mapping failed");
  constexpr std::uint32_t returned_instance = 7;
  require(
      server_memory.write32(server_buffer, complex_move_send_once) &&
          server_memory.write32(server_buffer + 4, 60) &&
          server_memory.write32(server_buffer + 8, configget_server_reply) &&
          server_memory.write32(server_buffer + 12, 0) &&
          server_memory.write32(server_buffer + 16, 0) &&
          server_memory.write32(server_buffer + 20, configget_id + 100U) &&
          server_memory.write32(server_buffer + 24, 1) &&
          server_memory.write32(server_buffer + 28, server_value) &&
          server_memory.write32(server_buffer + 32,
                                static_cast<std::uint32_t>(value.size())) &&
          server_memory.write32(server_buffer + 36,
                                deallocate_ool_descriptor) &&
          server_memory.write32(server_buffer + 40, 0) &&
          server_memory.write32(server_buffer + 44, 1) &&
          server_memory.write32(server_buffer + 48,
                                static_cast<std::uint32_t>(value.size())) &&
          server_memory.write32(server_buffer + 52, returned_instance) &&
          server_memory.write32(server_buffer + 56, 0),
      "configget complex OOL reply setup failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = mach_send_message;
  server_cpu.registers()[2] = 60;
  server_cpu.registers()[3] = 0;
  server_cpu.registers()[4] = 0;
  server.dispatch(server_cpu, 0x80);
  require(server_cpu.registers()[0] == 0 && !server_memory.mapped(server_value),
          "configget reply send did not deallocate server OOL memory");
  require(client.deliver_pending_mach(client_cpu),
          "configget OOL reply did not wake client");
  const auto client_value =
      client_memory.read32(client_buffer + 28).value_or(0);
  require(client_value != 0 && client_value != server_value &&
              client_memory.read_bytes(client_value, value.size()) ==
                  std::optional<std::vector<std::byte>>{
                      std::vector<std::byte>{value.begin(), value.end()}} &&
              client_memory.read32(client_buffer + 48) ==
                  std::optional<std::uint32_t>{
                      static_cast<std::uint32_t>(value.size())} &&
              client_memory.read32(client_buffer + 52) ==
                  std::optional<std::uint32_t>{returned_instance} &&
              client_memory.read32(client_buffer + 56) ==
                  std::optional<std::uint32_t>{0},
          "configget OOL payload, count, instance, or status mismatch");
}

void system_configuration_notification_mach_ipc_test() {
  constexpr std::uint32_t mach_send_message = 1;
  constexpr std::uint32_t mach_receive_message = 2;
  constexpr std::uint32_t mach_send_receive =
      mach_send_message | mach_receive_message;
  constexpr std::uint32_t mach_send_timeout = 0x10U;
  constexpr auto complex_copy_send_make_send_once =
      darwin::mig_wire::message_bits(
          darwin::mig_wire::disposition_copy_send,
          darwin::mig_wire::disposition_make_send_once, true);
  constexpr auto move_send_port_descriptor =
      darwin::mig_wire::port_descriptor_metadata(
          darwin::mig_wire::disposition_move_send);
  constexpr std::uint32_t kernel_reply_name = 0x900U;
  constexpr std::uint32_t notification_identifier = 0x53434401U;
  constexpr std::uint32_t buffer_size = 128;
  constexpr std::uint32_t server_buffer = 0x61000;
  constexpr std::uint32_t client_buffer = 0x62000;

  AddressSpace server_memory;
  AddressSpace client_memory;
  require(
      server_memory.map(server_buffer, AddressSpace::page_size,
                        MemoryPermission::Read | MemoryPermission::Write) &&
          client_memory.map(client_buffer, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write),
      "SystemConfiguration notification buffers failed to map");
  Dynarmic::ExclusiveMonitor server_monitor{1};
  Dynarmic::ExclusiveMonitor client_monitor{1};
  Cpu server_cpu{0, server_memory, server_monitor};
  Cpu client_cpu{0, client_memory, client_monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel server{server_memory, output};
  CompatibilityKernel client{client_memory, output};
  client.inherit_process_state(server, 2);

  const auto allocate_receive = [](CompatibilityKernel &kernel, Cpu &cpu) {
    cpu.registers()[12] = static_cast<std::uint32_t>(-26);
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] != 0,
            "SystemConfiguration receive-right allocation failed");
    return cpu.registers()[0];
  };
  const auto kernel_rpc = [&](CompatibilityKernel &kernel, Cpu &cpu,
                              std::uint32_t buffer, std::uint32_t send_size) {
    cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    cpu.registers()[0] = buffer;
    cpu.registers()[1] = mach_send_receive;
    cpu.registers()[2] = send_size;
    cpu.registers()[3] = buffer_size;
    cpu.registers()[4] = kernel_reply_name;
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == 0,
            "SystemConfiguration kernel MIG RPC failed");
  };
  const auto insert_send_right = [&](CompatibilityKernel &kernel, Cpu &cpu,
                                     AddressSpace &memory, std::uint32_t buffer,
                                     std::uint32_t receive_name) {
    const auto identifier = xnu792::mig::mach_port::id(
        xnu792::mig::mach_port::Routine::mach_port_insert_right);
    require(memory.write32(buffer, complex_copy_send_make_send_once) &&
                memory.write32(buffer + 4, 52) &&
                memory.write32(buffer + 8, kernel.process().task_port) &&
                memory.write32(buffer + 12, kernel_reply_name) &&
                memory.write32(buffer + 20, identifier) &&
                memory.write32(buffer + 24, 1) &&
                memory.write32(buffer + 28, receive_name) &&
                memory.write32(buffer + 32, 0) &&
                memory.write32(buffer + 36, 0x00140000U) &&
                memory.write32(buffer + 40, 0) &&
                memory.write32(buffer + 44, 1) &&
                memory.write32(buffer + 48, receive_name),
            "SystemConfiguration send-right insertion setup failed");
    kernel_rpc(kernel, cpu, buffer, 52);
    require(memory.read32(buffer + 32) == std::optional<std::uint32_t>{0},
            "SystemConfiguration send-right insertion failed");
  };
  const auto query_port_type = [&](CompatibilityKernel &kernel, Cpu &cpu,
                                   AddressSpace &memory, std::uint32_t buffer,
                                   std::uint32_t name) {
    const auto identifier = xnu792::mig::mach_port::id(
        xnu792::mig::mach_port::Routine::mach_port_type);
    require(memory.write32(buffer, 0x1513U) && memory.write32(buffer + 4, 36) &&
                memory.write32(buffer + 8, kernel.process().task_port) &&
                memory.write32(buffer + 12, kernel_reply_name) &&
                memory.write32(buffer + 20, identifier) &&
                memory.write32(buffer + 32, name),
            "SystemConfiguration port-type request setup failed");
    kernel_rpc(kernel, cpu, buffer, 36);
    require(memory.read32(buffer + 32) == std::optional<std::uint32_t>{0},
            "SystemConfiguration port-type query failed");
    return memory.read32(buffer + 36).value_or(0);
  };

  const auto client_reply_port = allocate_receive(client, client_cpu);
  const auto client_notify_port = allocate_receive(client, client_cpu);
  insert_send_right(client, client_cpu, client_memory, client_buffer,
                    client_notify_port);

  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] =
      mach_receive_message | (darwin::mig_wire::trailer_audit
                              << darwin::mig_wire::trailer_elements_shift);
  server_cpu.registers()[2] = 0;
  server_cpu.registers()[3] = buffer_size;
  server_cpu.registers()[4] = server.process().bootstrap_port;
  server.dispatch(server_cpu, 0x80);

  const auto notifyviaport_id = xnu792::mig::system_configuration::id(
      xnu792::mig::system_configuration::Routine::notifyviaport);
  require(
      client_memory.write32(client_buffer, complex_copy_send_make_send_once) &&
          client_memory.write32(client_buffer + 4, 52) &&
          client_memory.write32(client_buffer + 8,
                                server.process().bootstrap_port) &&
          client_memory.write32(client_buffer + 12, client_reply_port) &&
          client_memory.write32(client_buffer + 20, notifyviaport_id) &&
          client_memory.write32(client_buffer + 24, 1) &&
          client_memory.write32(client_buffer + 28, client_notify_port) &&
          client_memory.write32(client_buffer + 32, 0) &&
          client_memory.write32(client_buffer + 36,
                                move_send_port_descriptor) &&
          client_memory.write32(client_buffer + 40, 0) &&
          client_memory.write32(client_buffer + 44, 1) &&
          client_memory.write32(client_buffer + 48, notification_identifier),
      "notifyviaport request setup failed");
  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = mach_send_message;
  client_cpu.registers()[2] = 52;
  client_cpu.registers()[3] = 0;
  client_cpu.registers()[4] = 0;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0, "notifyviaport request send failed");
  require(server.deliver_pending_mach(server_cpu),
          "notifyviaport request did not wake configd task");

  const auto server_reply_port =
      server_memory.read32(server_buffer + 8).value_or(0);
  const auto server_notify_port =
      server_memory.read32(server_buffer + 28).value_or(0);
  require(server_reply_port != 0 && server_notify_port != 0 &&
              server_memory.read32(server_buffer + 4) ==
                  std::optional<std::uint32_t>{52} &&
              server_memory.read32(server_buffer + 20) ==
                  std::optional<std::uint32_t>{notifyviaport_id} &&
              server_memory.read32(server_buffer + 36) ==
                  std::optional<std::uint32_t>{move_send_port_descriptor} &&
              server_memory.read32(server_buffer + 48) ==
                  std::optional<std::uint32_t>{notification_identifier} &&
              stream.str().find("mig=config.notifyviaport") !=
                  std::string::npos,
          "notifyviaport request copyout or MIG layout mismatch");

  require(query_port_type(client, client_cpu, client_memory, client_buffer,
                          client_notify_port) ==
                  xnu792::ipc::type_mask(xnu792::ipc::Right::Receive) &&
              query_port_type(server, server_cpu, server_memory, server_buffer,
                              server_notify_port) ==
                  xnu792::ipc::type_mask(xnu792::ipc::Right::Send),
          "notifyviaport MOVE_SEND ownership mismatch");

  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = mach_receive_message;
  client_cpu.registers()[2] = 0;
  client_cpu.registers()[3] = buffer_size;
  client_cpu.registers()[4] = client_reply_port;
  client.dispatch(client_cpu, 0x80);

  require(
      server_memory.write32(server_buffer, 18) &&
          server_memory.write32(server_buffer + 4, 40) &&
          server_memory.write32(server_buffer + 8, server_reply_port) &&
          server_memory.write32(server_buffer + 12, 0) &&
          server_memory.write32(server_buffer + 16, 0) &&
          server_memory.write32(server_buffer + 20, notifyviaport_id + 100U) &&
          server_memory.write32(server_buffer + 24, 0) &&
          server_memory.write32(server_buffer + 28, 1) &&
          server_memory.write32(server_buffer + 32, 0) &&
          server_memory.write32(server_buffer + 36, 0),
      "notifyviaport reply setup failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = mach_send_message;
  server_cpu.registers()[2] = 40;
  server_cpu.registers()[3] = 0;
  server_cpu.registers()[4] = 0;
  server.dispatch(server_cpu, 0x80);
  require(server_cpu.registers()[0] == 0 &&
              client.deliver_pending_mach(client_cpu) &&
              client_memory.read32(client_buffer + 20) ==
                  std::optional<std::uint32_t>{notifyviaport_id + 100U} &&
              client_memory.read32(client_buffer + 36) ==
                  std::optional<std::uint32_t>{0},
          "notifyviaport reply did not complete client RPC");

  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = mach_receive_message;
  client_cpu.registers()[2] = 0;
  client_cpu.registers()[3] = buffer_size;
  client_cpu.registers()[4] = client_notify_port;
  client.dispatch(client_cpu, 0x80);

  require(
      server_memory.write32(server_buffer, 19) &&
          server_memory.write32(server_buffer + 4, 24) &&
          server_memory.write32(server_buffer + 8, server_notify_port) &&
          server_memory.write32(server_buffer + 12, 0) &&
          server_memory.write32(server_buffer + 16, 0) &&
          server_memory.write32(server_buffer + 20, notification_identifier),
      "configd notification message setup failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = mach_send_message | mach_send_timeout;
  server_cpu.registers()[2] = 24;
  server_cpu.registers()[3] = 0;
  server_cpu.registers()[4] = 0;
  server.dispatch(server_cpu, 0x80);
  require(server_cpu.registers()[0] == 0 &&
              client.deliver_pending_mach(client_cpu) &&
              client_memory.read32(client_buffer + 12) ==
                  std::optional<std::uint32_t>{client_notify_port} &&
              client_memory.read32(client_buffer + 20) ==
                  std::optional<std::uint32_t>{notification_identifier},
          "configd Mach notification callback was not delivered");
}

void system_configuration_watched_key_mach_ipc_test() {
  using namespace darwin::mig_wire;
  using namespace xnu792::mig::system_configuration;
  constexpr std::uint32_t mach_send_message = 1;
  constexpr std::uint32_t mach_receive_message = 2;
  constexpr std::uint32_t server_buffer = 0x63000;
  constexpr std::uint32_t client_buffer = 0x64000;
  constexpr std::uint32_t client_key = 0x65000;
  constexpr std::uint32_t client_data = 0x66000;
  constexpr std::uint32_t server_changes = 0x67000;
  constexpr std::uint32_t message_capacity = 128;
  constexpr std::uint32_t mig_reply_id_delta = 100;
  constexpr std::uint32_t ndr_byte_order_word = 1;
  const std::array<std::byte, 6> key{std::byte{'<'}, std::byte{'k'},
                                     std::byte{'e'}, std::byte{'y'},
                                     std::byte{'/'}, std::byte{'>'}};
  const std::array<std::byte, 7> value{
      std::byte{'<'}, std::byte{'d'}, std::byte{'a'}, std::byte{'t'},
      std::byte{'a'}, std::byte{'/'}, std::byte{'>'}};
  const std::array<std::byte, 9> changes{
      std::byte{'<'}, std::byte{'c'}, std::byte{'h'},
      std::byte{'a'}, std::byte{'n'}, std::byte{'g'},
      std::byte{'e'}, std::byte{'/'}, std::byte{'>'}};

  AddressSpace server_memory;
  AddressSpace client_memory;
  require(
      server_memory.map(server_buffer, AddressSpace::page_size,
                        MemoryPermission::Read | MemoryPermission::Write) &&
          server_memory.map(server_changes, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write) &&
          client_memory.map(client_buffer, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write) &&
          client_memory.map(client_key, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write) &&
          client_memory.map(client_data, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write) &&
          client_memory.copy_in(client_key, key) &&
          client_memory.copy_in(client_data, value) &&
          server_memory.copy_in(server_changes, changes),
      "watched-key Mach IPC fixture mapping failed");
  Dynarmic::ExclusiveMonitor server_monitor{1};
  Dynarmic::ExclusiveMonitor client_monitor{1};
  Cpu server_cpu{0, server_memory, server_monitor};
  Cpu client_cpu{0, client_memory, client_monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel server{server_memory, output};
  CompatibilityKernel client{client_memory, output};
  client.inherit_process_state(server, 2);

  const auto start_rpc = [&](Routine routine, std::uint32_t request_size,
                             bool complex) {
    client_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
    client.dispatch(client_cpu, 0x80);
    const auto reply_receive = client_cpu.registers()[0];
    require(reply_receive != 0,
            "watched-key reply receive-right allocation failed");

    server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    server_cpu.registers()[0] = server_buffer;
    server_cpu.registers()[1] =
        mach_receive_message | (trailer_audit << trailer_elements_shift);
    server_cpu.registers()[2] = 0;
    server_cpu.registers()[3] = message_capacity;
    server_cpu.registers()[4] = server.process().bootstrap_port;
    server.dispatch(server_cpu, 0x80);

    require(client_memory.write32(client_buffer,
                                  message_bits(disposition_copy_send,
                                               disposition_make_send_once,
                                               complex)) &&
                client_memory.write32(client_buffer + 4, request_size) &&
                client_memory.write32(client_buffer + 8,
                                      client.process().bootstrap_port) &&
                client_memory.write32(client_buffer + 12, reply_receive) &&
                client_memory.write32(client_buffer + 16, 0) &&
                client_memory.write32(client_buffer + 20, id(routine)),
            "watched-key Mach request header setup failed");
    client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    client_cpu.registers()[0] = client_buffer;
    client_cpu.registers()[1] = mach_send_message;
    client_cpu.registers()[2] = request_size;
    client_cpu.registers()[3] = 0;
    client_cpu.registers()[4] = 0;
    client.dispatch(client_cpu, 0x80);
    require(client_cpu.registers()[0] == 0 &&
                server.deliver_pending_mach(server_cpu),
            "watched-key request did not reach configd task");
    const auto server_reply =
        server_memory.read32(server_buffer + 8).value_or(0);
    require(server_reply != 0 &&
                server_memory.read32(server_buffer + 20) ==
                    std::optional<std::uint32_t>{id(routine)} &&
                server_memory.read32(server_buffer + request_size) ==
                    std::optional<std::uint32_t>{0} &&
                server_memory.read32(server_buffer + request_size + 4) ==
                    std::optional<std::uint32_t>{trailer_audit_size},
            "watched-key request header or audit trailer mismatch");

    client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    client_cpu.registers()[0] = client_buffer;
    client_cpu.registers()[1] = mach_receive_message;
    client_cpu.registers()[2] = 0;
    client_cpu.registers()[3] = message_capacity;
    client_cpu.registers()[4] = reply_receive;
    client.dispatch(client_cpu, 0x80);
    return server_reply;
  };
  const auto send_reply = [&](Routine routine, std::uint32_t reply_size,
                              std::uint32_t server_reply) {
    require(server_memory.write32(server_buffer + 4, reply_size) &&
                server_memory.write32(server_buffer + 8, server_reply) &&
                server_memory.write32(server_buffer + 12, 0) &&
                server_memory.write32(server_buffer + 16, 0) &&
                server_memory.write32(server_buffer + 20,
                                      id(routine) + mig_reply_id_delta),
            "watched-key reply header setup failed");
    server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    server_cpu.registers()[0] = server_buffer;
    server_cpu.registers()[1] = mach_send_message;
    server_cpu.registers()[2] = reply_size;
    server_cpu.registers()[3] = 0;
    server_cpu.registers()[4] = 0;
    server.dispatch(server_cpu, 0x80);
    require(server_cpu.registers()[0] == 0 &&
                client.deliver_pending_mach(client_cpu),
            "watched-key reply did not reach framework task");
  };

  const auto build_single_ool_request =
      [&](std::uint32_t address, std::uint32_t size,
          const xnu792::mig::ArgumentInfo &argument) {
        require(
            client_memory.write32(
                client_buffer + complex_descriptor_count_offset, 1) &&
                client_memory.write32(client_buffer + argument.request_offset,
                                      address) &&
                client_memory.write32(client_buffer + argument.request_offset +
                                          word_size,
                                      size) &&
                client_memory.write32(client_buffer + argument.request_offset +
                                          2U * word_size,
                                      ool_descriptor_metadata(false)) &&
                client_memory.write32(client_buffer + complex_ndr_offset(1),
                                      0) &&
                client_memory.write32(client_buffer + complex_ndr_offset(1) +
                                          word_size,
                                      ndr_byte_order_word) &&
                client_memory.write32(
                    client_buffer + argument.request_count_offset, size),
            "watched-key single OOL request setup failed");
      };

  constexpr std::uint32_t notifyadd_request_size = 56;
  build_single_ool_request(client_key, static_cast<std::uint32_t>(key.size()),
                           notifyadd_arguments[1]);
  require(client_memory.write32(
              client_buffer + notifyadd_arguments[2].request_offset, 0),
          "notifyadd regular-key flag setup failed");
  const auto notifyadd_reply =
      start_rpc(Routine::notifyadd, notifyadd_request_size, true);
  const auto notify_key_copy =
      server_memory
          .read32(server_buffer + notifyadd_arguments[1].request_offset)
          .value_or(0);
  require(
      notify_key_copy != 0 &&
          server_memory.read_bytes(notify_key_copy, key.size()) ==
              std::optional<std::vector<std::byte>>{
                  std::vector<std::byte>{key.begin(), key.end()}} &&
          server_memory.read32(server_buffer +
                               notifyadd_arguments[1].request_count_offset) ==
              std::optional<std::uint32_t>{
                  static_cast<std::uint32_t>(key.size())} &&
          server_memory.read32(server_buffer +
                               notifyadd_arguments[2].request_offset) ==
              std::optional<std::uint32_t>{0},
      "notifyadd watched-key payload mismatch");
  constexpr std::uint32_t notifyadd_reply_size = 40;
  require(server_memory.write32(server_buffer,
                                message_bits(disposition_move_send_once)) &&
              server_memory.write32(server_buffer + 24, 0) &&
              server_memory.write32(server_buffer + 28, ndr_byte_order_word) &&
              server_memory.write32(server_buffer + 32, 0) &&
              server_memory.write32(
                  server_buffer + notifyadd_arguments[3].reply_offset, 0),
          "notifyadd reply payload setup failed");
  send_reply(Routine::notifyadd, notifyadd_reply_size, notifyadd_reply);
  require(client_memory.read32(client_buffer +
                               notifyadd_arguments[3].reply_offset) ==
              std::optional<std::uint32_t>{0},
          "notifyadd framework status mismatch");

  constexpr std::uint32_t configset_request_size = 72;
  require(
      client_memory.write32(client_buffer + complex_descriptor_count_offset, 2),
      "configset descriptor count setup failed");
  const auto write_configset_ool = [&](std::size_t argument_index,
                                       std::uint32_t address,
                                       std::uint32_t size) {
    const auto &argument = configset_arguments[argument_index];
    return client_memory.write32(client_buffer + argument.request_offset,
                                 address) &&
           client_memory.write32(
               client_buffer + argument.request_offset + word_size, size) &&
           client_memory.write32(client_buffer + argument.request_offset +
                                     2U * word_size,
                                 ool_descriptor_metadata(false)) &&
           client_memory.write32(client_buffer + argument.request_count_offset,
                                 size);
  };
  require(write_configset_ool(1, client_key,
                              static_cast<std::uint32_t>(key.size())) &&
              write_configset_ool(2, client_data,
                                  static_cast<std::uint32_t>(value.size())) &&
              client_memory.write32(client_buffer + complex_ndr_offset(2), 0) &&
              client_memory.write32(client_buffer + complex_ndr_offset(2) +
                                        word_size,
                                    ndr_byte_order_word) &&
              client_memory.write32(
                  client_buffer + configset_arguments[3].request_offset, 0),
          "configset OOL/scalar request setup failed");
  const auto configset_reply =
      start_rpc(Routine::configset, configset_request_size, true);
  const auto configset_key_copy =
      server_memory
          .read32(server_buffer + configset_arguments[1].request_offset)
          .value_or(0);
  const auto configset_value_copy =
      server_memory
          .read32(server_buffer + configset_arguments[2].request_offset)
          .value_or(0);
  require(configset_key_copy != 0 && configset_value_copy != 0 &&
              server_memory.read_bytes(configset_key_copy, key.size()) ==
                  std::optional<std::vector<std::byte>>{
                      std::vector<std::byte>{key.begin(), key.end()}} &&
              server_memory.read_bytes(configset_value_copy, value.size()) ==
                  std::optional<std::vector<std::byte>>{
                      std::vector<std::byte>{value.begin(), value.end()}} &&
              server_memory.read32(server_buffer +
                                   configset_arguments[3].request_offset) ==
                  std::optional<std::uint32_t>{0},
          "configset key/data/instance copyin mismatch");
  constexpr std::uint32_t configset_reply_size = 44;
  constexpr std::uint32_t new_instance = 1;
  require(server_memory.write32(server_buffer,
                                message_bits(disposition_move_send_once)) &&
              server_memory.write32(server_buffer + 24, 0) &&
              server_memory.write32(server_buffer + 28, ndr_byte_order_word) &&
              server_memory.write32(server_buffer + 32, 0) &&
              server_memory.write32(server_buffer +
                                        configset_arguments[4].reply_offset,
                                    new_instance) &&
              server_memory.write32(
                  server_buffer + configset_arguments[5].reply_offset, 0),
          "configset reply payload setup failed");
  send_reply(Routine::configset, configset_reply_size, configset_reply);
  require(client_memory.read32(client_buffer +
                               configset_arguments[4].reply_offset) ==
                  std::optional<std::uint32_t>{new_instance} &&
              client_memory.read32(client_buffer +
                                   configset_arguments[5].reply_offset) ==
                  std::optional<std::uint32_t>{0},
          "configset new-instance/status reply mismatch");

  constexpr std::uint32_t notifychanges_request_size = 24;
  const auto notifychanges_reply =
      start_rpc(Routine::notifychanges, notifychanges_request_size, false);
  constexpr std::uint32_t notifychanges_reply_size = 56;
  require(
      server_memory.write32(
          server_buffer, message_bits(disposition_move_send_once, 0, true)) &&
          server_memory.write32(server_buffer + complex_descriptor_count_offset,
                                1) &&
          server_memory.write32(server_buffer +
                                    notifychanges_arguments[1].reply_offset,
                                server_changes) &&
          server_memory.write32(server_buffer +
                                    notifychanges_arguments[1].reply_offset +
                                    word_size,
                                static_cast<std::uint32_t>(changes.size())) &&
          server_memory.write32(server_buffer +
                                    notifychanges_arguments[1].reply_offset +
                                    2U * word_size,
                                ool_descriptor_metadata(true)) &&
          server_memory.write32(server_buffer + complex_ndr_offset(1), 0) &&
          server_memory.write32(server_buffer + complex_ndr_offset(1) +
                                    word_size,
                                ndr_byte_order_word) &&
          server_memory.write32(
              server_buffer + notifychanges_arguments[1].reply_count_offset,
              static_cast<std::uint32_t>(changes.size())) &&
          server_memory.write32(
              server_buffer + notifychanges_arguments[2].reply_offset, 0),
      "notifychanges complex OOL reply setup failed");
  send_reply(Routine::notifychanges, notifychanges_reply_size,
             notifychanges_reply);
  require(!server_memory.mapped(server_changes),
          "notifychanges deallocated OOL reply retained server VM");
  const auto client_changes =
      client_memory
          .read32(client_buffer + notifychanges_arguments[1].reply_offset)
          .value_or(0);
  require(
      client_changes != 0 && client_changes != server_changes &&
          client_memory.read_bytes(client_changes, changes.size()) ==
              std::optional<std::vector<std::byte>>{
                  std::vector<std::byte>{changes.begin(), changes.end()}} &&
          client_memory.read32(client_buffer +
                               notifychanges_arguments[1].reply_count_offset) ==
              std::optional<std::uint32_t>{
                  static_cast<std::uint32_t>(changes.size())} &&
          client_memory.read32(client_buffer +
                               notifychanges_arguments[2].reply_offset) ==
              std::optional<std::uint32_t>{0} &&
          stream.str().find("mig=config.notifyadd") != std::string::npos &&
          stream.str().find("mig=config.configset") != std::string::npos &&
          stream.str().find("mig=config.notifychanges") != std::string::npos,
          "notifychanges payload/status or named MIG trace mismatch");
}

} // namespace

void run_ipc_ool_tests() {
  cross_process_ool_mach_ipc_test();
  system_configuration_notification_mach_ipc_test();
  system_configuration_watched_key_mach_ipc_test();
}

} // namespace ilemu::test::mach_suite
