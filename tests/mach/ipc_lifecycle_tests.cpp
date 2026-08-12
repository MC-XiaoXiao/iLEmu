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

void cross_process_mach_ipc_test() {
  AddressSpace server_memory;
  AddressSpace client_memory;
  constexpr std::uint32_t server_buffer = 0x10000;
  constexpr std::uint32_t client_buffer = 0x20000;
  require(server_memory.map(server_buffer, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write),
          "server IPC buffer map failed");
  require(client_memory.map(client_buffer, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write),
          "client IPC buffer map failed");

  Dynarmic::ExclusiveMonitor server_monitor{1};
  Dynarmic::ExclusiveMonitor client_monitor{1};
  Cpu server_cpu{0, server_memory, server_monitor};
  Cpu client_cpu{0, client_memory, client_monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel server{server_memory, output};
  CompatibilityKernel client{client_memory, output};
  client.inherit_process_state(server, 2);

  require(server_memory.write32(server_buffer, 0x1513),
          "allocate bits write failed");
  require(server_memory.write32(server_buffer + 4, 36),
          "allocate size write failed");
  require(server_memory.write32(server_buffer + 8, server.process().task_port),
          "allocate destination write failed");
  require(server_memory.write32(server_buffer + 12, 0x800),
          "allocate reply write failed");
  require(server_memory.write32(server_buffer + 20, 3204),
          "allocate id write failed");
  require(server_memory.write32(server_buffer + 32, 3),
          "port-set right write failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 36;
  server_cpu.registers()[3] = 40;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  const auto port_set = server_memory.read32(server_buffer + 36).value_or(0);
  require(port_set >= 0x10000, "port set was not allocated");

  require(server_memory.write32(server_buffer, 0x1513),
          "second allocate bits write failed");
  require(server_memory.write32(server_buffer + 4, 36),
          "second allocate size write failed");
  require(server_memory.write32(server_buffer + 8, server.process().task_port),
          "second allocate destination write failed");
  require(server_memory.write32(server_buffer + 12, 0x800),
          "second allocate reply write failed");
  require(server_memory.write32(server_buffer + 20, 3204),
          "second allocate id write failed");
  require(server_memory.write32(server_buffer + 32, 1),
          "receive-right write failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 36;
  server_cpu.registers()[3] = 40;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  const auto receive_port =
      server_memory.read32(server_buffer + 36).value_or(0);
  require(receive_port >= 0x10000, "receive right was not allocated");
  require((port_set >> 8U) != (receive_port >> 8U),
          "consecutive Mach names reused one ipc-space index");

  // mach_port_type operates on the destination task's ipc_space. The child
  // was created before this server-only receive right, so the same numeric
  // name must not become visible merely because the port object is global.
  require(client_memory.write32(client_buffer, 0x1513) &&
              client_memory.write32(client_buffer + 4, 36) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x801) &&
              client_memory.write32(client_buffer + 20, 3201) &&
              client_memory.write32(client_buffer + 32, receive_port),
          "child namespace probe message setup failed");
  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = 3;
  client_cpu.registers()[2] = 36;
  client_cpu.registers()[3] = 40;
  client_cpu.registers()[4] = 0x801;
  client.dispatch(client_cpu, 0x80);
  require(client_memory.read32(client_buffer + 32) ==
                  std::optional<std::uint32_t>{15} &&
              client_memory.read32(client_buffer + 36) ==
                  std::optional<std::uint32_t>{0},
          "mach_port_type leaked a server right into the child ipc_space");

  // A same-valued global object identifier is not a capability in the
  // child's ipc_space and must therefore fail generic message routing.
  require(client_memory.write32(client_buffer, 0x13) &&
              client_memory.write32(client_buffer + 4, 24) &&
              client_memory.write32(client_buffer + 8, receive_port) &&
              client_memory.write32(client_buffer + 12, 0) &&
              client_memory.write32(client_buffer + 16, 0) &&
              client_memory.write32(client_buffer + 20, 999),
          "raw-object destination probe setup failed");
  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = 1; // MACH_SEND_MSG
  client_cpu.registers()[2] = 24;
  client_cpu.registers()[3] = 0;
  client_cpu.registers()[4] = 0;
  client.dispatch(client_cpu, 0x80);
  const auto invalid_name_halt = client_cpu.consume_requested_halt_reason();
  require(
      client_cpu.registers()[0] == 0x10000003U &&
          !Dynarmic::Has(invalid_name_halt, Dynarmic::HaltReason::UserDefined4),
      "invalid destination was accepted or treated as an unknown trap");

  // MIG servers may construct an ID+100 reply and then clear its destination
  // for MIG_NO_REPLY. XNU returns MACH_SEND_INVALID_DEST without treating the
  // reply as an unknown kernel RPC.
  require(client_memory.write32(client_buffer, 0) &&
              client_memory.write32(client_buffer + 4, 36) &&
              client_memory.write32(client_buffer + 8, 0) &&
              client_memory.write32(client_buffer + 12, 0) &&
              client_memory.write32(client_buffer + 20, 171) &&
              client_memory.write32(client_buffer + 24, 0) &&
              client_memory.write32(client_buffer + 28, 1) &&
              client_memory.write32(client_buffer + 32, 0xfffffed1U),
          "MIG_NO_REPLY null-destination reply setup failed");
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = 0x11U; // MACH_SEND_MSG | MACH_SEND_TIMEOUT
  client_cpu.registers()[2] = 36;
  client.dispatch(client_cpu, 0x80);
  const auto no_reply_halt = client_cpu.consume_requested_halt_reason();
  require(client_cpu.registers()[0] == 0x10000003U &&
              !Dynarmic::Has(no_reply_halt, Dynarmic::HaltReason::UserDefined4),
          "MIG_NO_REPLY null destination halted the guest");

  require(server_memory.write32(server_buffer, 0x1513),
          "set-mscount bits write failed");
  require(server_memory.write32(server_buffer + 4, 40),
          "set-mscount size write failed");
  require(server_memory.write32(server_buffer + 8, server.process().task_port),
          "set-mscount destination write failed");
  require(server_memory.write32(server_buffer + 12, 0x800),
          "set-mscount reply write failed");
  require(server_memory.write32(server_buffer + 20, 3210),
          "set-mscount id write failed");
  require(server_memory.write32(server_buffer + 32, receive_port),
          "set-mscount name write failed");
  require(server_memory.write32(server_buffer + 36, 7),
          "set-mscount value write failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 40;
  server_cpu.registers()[3] = 44;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  require(server_memory.read32(server_buffer + 32) ==
              std::optional<std::uint32_t>{0},
          "mach_port_set_mscount failed");

  require(server_memory.write32(server_buffer, 0x1513),
          "port-type bits write failed");
  require(server_memory.write32(server_buffer + 4, 36),
          "port-type size write failed");
  require(server_memory.write32(server_buffer + 8, server.process().task_port),
          "port-type destination write failed");
  require(server_memory.write32(server_buffer + 12, 0x800),
          "port-type reply write failed");
  require(server_memory.write32(server_buffer + 20, 3201),
          "port-type id write failed");
  require(server_memory.write32(server_buffer + 32, receive_port),
          "port-type name write failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 36;
  server_cpu.registers()[3] = 40;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  require(server_memory.read32(server_buffer + 32) ==
              std::optional<std::uint32_t>{0},
          "mach_port_type failed");
  require(server_memory.read32(server_buffer + 36) ==
              std::optional<std::uint32_t>{0x00020000U},
          "mach_port_type did not report a receive right");

  require(server_memory.write32(server_buffer, 0x80001513U) &&
              server_memory.write32(server_buffer + 4, 52) &&
              server_memory.write32(server_buffer + 8,
                                    server.process().task_port) &&
              server_memory.write32(server_buffer + 12, 0x800) &&
              server_memory.write32(server_buffer + 20, 3214) &&
              server_memory.write32(server_buffer + 24, 1) &&
              server_memory.write32(server_buffer + 28, receive_port) &&
              server_memory.write32(server_buffer + 36, 0x00140000U) &&
              server_memory.write32(server_buffer + 40, 0) &&
              server_memory.write32(server_buffer + 44, 1) &&
              server_memory.write32(server_buffer + 48, receive_port),
          "mach_port_insert_right request setup failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 52;
  server_cpu.registers()[3] = 40;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  require(server_memory.read32(server_buffer + 32) ==
              std::optional<std::uint32_t>{0},
          "mach_port_insert_right failed");

  require(server_memory.write32(server_buffer, 0x1513) &&
              server_memory.write32(server_buffer + 4, 36) &&
              server_memory.write32(server_buffer + 8,
                                    server.process().task_port) &&
              server_memory.write32(server_buffer + 12, 0x800) &&
              server_memory.write32(server_buffer + 20, 3201) &&
              server_memory.write32(server_buffer + 32, receive_port),
          "composite port-type request setup failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 36;
  server_cpu.registers()[3] = 40;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  require(server_memory.read32(server_buffer + 36) ==
              std::optional<std::uint32_t>{0x00030000U},
          "inserted send right did not form a composite Mach type");

  require(server_memory.write32(server_buffer, 0x1513) &&
              server_memory.write32(server_buffer + 4, 36) &&
              server_memory.write32(server_buffer + 8,
                                    server.process().task_port) &&
              server_memory.write32(server_buffer + 12, 0x800) &&
              server_memory.write32(server_buffer + 20, 3206) &&
              server_memory.write32(server_buffer + 32, receive_port),
          "mach_port_deallocate request setup failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 36;
  server_cpu.registers()[3] = 40;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  require(server_memory.read32(server_buffer + 32) ==
              std::optional<std::uint32_t>{0},
          "mach_port_deallocate failed for a send user reference");

  require(server_memory.write32(server_buffer, 0x1513) &&
              server_memory.write32(server_buffer + 4, 36) &&
              server_memory.write32(server_buffer + 8,
                                    server.process().task_port) &&
              server_memory.write32(server_buffer + 12, 0x800) &&
              server_memory.write32(server_buffer + 20, 3201) &&
              server_memory.write32(server_buffer + 32, receive_port),
          "post-deallocate port-type request setup failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 36;
  server_cpu.registers()[3] = 40;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  require(server_memory.read32(server_buffer + 36) ==
              std::optional<std::uint32_t>{0x00020000U},
          "deallocation removed the receive half of a composite right");

  // Recreate a send right so no-senders registration remains pending until
  // the final send user reference is deallocated below.
  require(server_memory.write32(server_buffer, 0x80001513U) &&
              server_memory.write32(server_buffer + 4, 52) &&
              server_memory.write32(server_buffer + 8,
                                    server.process().task_port) &&
              server_memory.write32(server_buffer + 12, 0x800) &&
              server_memory.write32(server_buffer + 20, 3214) &&
              server_memory.write32(server_buffer + 24, 1) &&
              server_memory.write32(server_buffer + 28, receive_port) &&
              server_memory.write32(server_buffer + 36, 0x00140000U) &&
              server_memory.write32(server_buffer + 40, 0) &&
              server_memory.write32(server_buffer + 44, 1) &&
              server_memory.write32(server_buffer + 48, receive_port),
          "second mach_port_insert_right request setup failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 52;
  server_cpu.registers()[3] = 40;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  require(server_memory.read32(server_buffer + 32) ==
              std::optional<std::uint32_t>{0},
          "second mach_port_insert_right failed");

  const auto request_no_senders_notification = [&] {
    require(server_memory.write32(server_buffer, 0x80001513U) &&
                server_memory.write32(server_buffer + 4, 60) &&
                server_memory.write32(server_buffer + 8,
                                      server.process().task_port) &&
                server_memory.write32(server_buffer + 12, 0x800) &&
                server_memory.write32(server_buffer + 20, 3213) &&
                server_memory.write32(server_buffer + 24, 1) &&
                server_memory.write32(server_buffer + 28, receive_port) &&
                server_memory.write32(server_buffer + 36, 0x00150000U) &&
                server_memory.write32(server_buffer + 40, 0) &&
                server_memory.write32(server_buffer + 44, 1) &&
                server_memory.write32(server_buffer + 48, receive_port) &&
                server_memory.write32(server_buffer + 52, 70) &&
                server_memory.write32(server_buffer + 56, 1),
            "mach_port_request_notification setup failed");
    server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    server_cpu.registers()[0] = server_buffer;
    server_cpu.registers()[1] = 3;
    server_cpu.registers()[2] = 60;
    server_cpu.registers()[3] = 40;
    server_cpu.registers()[4] = 0x800;
    server.dispatch(server_cpu, 0x80);
    require(server_cpu.registers()[0] == 0 &&
                server_memory.read32(server_buffer + 4) ==
                    std::optional<std::uint32_t>{40},
            "mach_port_request_notification failed");
    return server_memory.read32(server_buffer + 28).value_or(0);
  };
  require(request_no_senders_notification() == 0,
          "first notification request returned a previous right");
  const auto previous_notification = request_no_senders_notification();
  require(previous_notification != 0 &&
              previous_notification != receive_port,
          "replacement notification did not return a distinct previous "
          "send-once right");

  require(server_memory.write32(server_buffer, 0x1513) &&
              server_memory.write32(server_buffer + 4, 36) &&
              server_memory.write32(server_buffer + 8,
                                    server.process().task_port) &&
              server_memory.write32(server_buffer + 12, 0x800) &&
              server_memory.write32(server_buffer + 20, 3206) &&
              server_memory.write32(server_buffer + 32, receive_port),
          "notification-triggering deallocate request setup failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 36;
  server_cpu.registers()[3] = 64;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  require(server_memory.read32(server_buffer + 32) ==
              std::optional<std::uint32_t>{0},
          "final send-right deallocation failed");

  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 2; // MACH_RCV_MSG
  server_cpu.registers()[2] = 0;
  server_cpu.registers()[3] = 64;
  server_cpu.registers()[4] = receive_port;
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server.dispatch(server_cpu, 0x80);
  require(server_cpu.registers()[0] == 0 &&
              server_memory.read32(server_buffer + 20) ==
                  std::optional<std::uint32_t>{70} &&
              server_memory.read32(server_buffer + 32) ==
                  std::optional<std::uint32_t>{9},
          "MACH_NOTIFY_NO_SENDERS was not delivered with the make-send count");

  require(request_no_senders_notification() == 0,
          "immediate no-senders registration returned a stale request");
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 2;
  server_cpu.registers()[2] = 0;
  server_cpu.registers()[3] = 64;
  server_cpu.registers()[4] = receive_port;
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server.dispatch(server_cpu, 0x80);
  require(server_cpu.registers()[0] == 0 &&
              server_memory.read32(server_buffer + 20) ==
                  std::optional<std::uint32_t>{70} &&
              server_memory.read32(server_buffer + 32) ==
                  std::optional<std::uint32_t>{9},
          "immediate MACH_NOTIFY_NO_SENDERS delivery failed");

  require(server_memory.write32(server_buffer, 0x1513),
          "move-member bits write failed");
  require(server_memory.write32(server_buffer + 4, 40),
          "move-member size write failed");
  require(server_memory.write32(server_buffer + 8, server.process().task_port),
          "move-member destination write failed");
  require(server_memory.write32(server_buffer + 12, 0x800),
          "move-member reply write failed");
  require(server_memory.write32(server_buffer + 20, 3212),
          "move-member id write failed");
  require(server_memory.write32(server_buffer + 32,
                                server.process().bootstrap_port),
          "move-member port write failed");
  require(server_memory.write32(server_buffer + 36, port_set),
          "move-member set write failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 40;
  server_cpu.registers()[3] = 40;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  require(server_memory.read32(server_buffer + 32) ==
              std::optional<std::uint32_t>{0},
          "mach_port_move_member failed");

  const auto call_port_membership = [&](std::uint32_t identifier,
                                        std::uint32_t requested_set = 0) {
    if (requested_set == 0)
      requested_set = port_set;
    require(server_memory.write32(server_buffer, 0x1513) &&
                server_memory.write32(server_buffer + 4, 40) &&
                server_memory.write32(server_buffer + 8,
                                      server.process().task_port) &&
                server_memory.write32(server_buffer + 12, 0x800) &&
                server_memory.write32(server_buffer + 20, identifier) &&
                server_memory.write32(server_buffer + 32,
                                      server.process().bootstrap_port) &&
                server_memory.write32(server_buffer + 36, requested_set),
            "port-membership request setup failed");
    server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    server_cpu.registers()[0] = server_buffer;
    server_cpu.registers()[1] = 3;
    server_cpu.registers()[2] = 40;
    server_cpu.registers()[3] = 40;
    server_cpu.registers()[4] = 0x800;
    server.dispatch(server_cpu, 0x80);
    return server_memory.read32(server_buffer + 32).value_or(0xffff'ffffU);
  };

  require(call_port_membership(3226) == 11,
          "duplicate mach_port_insert_member result mismatch");
  require(call_port_membership(3227) == 0, "mach_port_extract_member failed");
  require(call_port_membership(3227) == 12,
          "duplicate mach_port_extract_member result mismatch");
  require(call_port_membership(3226) == 0,
          "mach_port_insert_member failed after extraction");

  require(server_memory.write32(server_buffer, 0x1513) &&
              server_memory.write32(server_buffer + 4, 36) &&
              server_memory.write32(server_buffer + 8,
                                    server.process().task_port) &&
              server_memory.write32(server_buffer + 12, 0x800) &&
              server_memory.write32(server_buffer + 20, 3204) &&
              server_memory.write32(server_buffer + 32, 3),
          "second port-set allocation setup failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 36;
  server_cpu.registers()[3] = 40;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  const auto second_port_set =
      server_memory.read32(server_buffer + 36).value_or(0);
  require(second_port_set != 0 && second_port_set != port_set &&
              call_port_membership(3226, second_port_set) == 0,
          "Darwin 8 receive right was not inserted into a second port set");

  require(server_memory.write32(server_buffer, 0x1513),
          "get-set-status bits write failed");
  require(server_memory.write32(server_buffer + 4, 36),
          "get-set-status size write failed");
  require(server_memory.write32(server_buffer + 8, server.process().task_port),
          "get-set-status destination write failed");
  require(server_memory.write32(server_buffer + 12, 0x800),
          "get-set-status reply write failed");
  require(server_memory.write32(server_buffer + 20, 3211),
          "firmware get-set-status id write failed");
  require(server_memory.write32(server_buffer + 32, port_set),
          "get-set-status name write failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 36;
  server_cpu.registers()[3] = 60;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  require(server_memory.read32(server_buffer + 4) ==
              std::optional<std::uint32_t>{52},
          "get-set-status reply size mismatch");
  require(server_memory.read32(server_buffer + 48) ==
              std::optional<std::uint32_t>{1},
          "get-set-status member count mismatch");
  const auto members_address =
      server_memory.read32(server_buffer + 28).value_or(0);
  require(members_address != 0 &&
              server_memory.read32(members_address) ==
                  std::optional<std::uint32_t>{server.process().bootstrap_port},
          "get-set-status member array mismatch");

  require(server_memory.write32(server_buffer, 0x1513),
          "receive-status bits write failed");
  require(server_memory.write32(server_buffer + 4, 44),
          "receive-status size write failed");
  require(server_memory.write32(server_buffer + 8, server.process().task_port),
          "receive-status destination write failed");
  require(server_memory.write32(server_buffer + 12, 0x800),
          "receive-status reply write failed");
  require(server_memory.write32(server_buffer + 20, 3217),
          "receive-status id write failed");
  require(server_memory.write32(server_buffer + 32,
                                server.process().bootstrap_port),
          "receive-status name write failed");
  require(server_memory.write32(server_buffer + 36, 2),
          "receive-status flavor write failed");
  require(server_memory.write32(server_buffer + 40, 10),
          "receive-status count write failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 3;
  server_cpu.registers()[2] = 44;
  server_cpu.registers()[3] = 88;
  server_cpu.registers()[4] = 0x800;
  server.dispatch(server_cpu, 0x80);
  require(server_memory.read32(server_buffer + 4) ==
              std::optional<std::uint32_t>{80},
          "receive-status reply size mismatch");
  require(server_memory.read32(server_buffer + 32) ==
              std::optional<std::uint32_t>{0},
          "receive-status request failed");
  require(server_memory.read32(server_buffer + 56) ==
              std::optional<std::uint32_t>{0},
          "empty receive right reported queued messages");
  require(server_memory.read32(server_buffer + 36) ==
              std::optional<std::uint32_t>{10},
          "receive-status output count mismatch");

  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 2; // MACH_RCV_MSG
  server_cpu.registers()[2] = 0;
  server_cpu.registers()[3] = 128;
  server_cpu.registers()[4] = port_set;
  server.dispatch(server_cpu, 0x80);

  // The server has already copied out the client task capability. Consume one
  // client-local slot so the next receive name deliberately collides with the
  // server-local port-set name while still naming a different IPC object.
  client_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] != 0,
          "reply-port collision cursor setup failed");
  client_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  client.dispatch(client_cpu, 0x80);
  const auto reply_port = client_cpu.registers()[0];
  require(reply_port == port_set,
          "independent task ipc_spaces did not reuse the first dynamic name");

  require(client_memory.write32(client_buffer, 0x1513),
          "request bits write failed");
  require(client_memory.write32(client_buffer + 4, 24),
          "request size write failed");
  require(
      client_memory.write32(client_buffer + 8, server.process().bootstrap_port),
      "request destination write failed");
  require(client_memory.write32(client_buffer + 12, reply_port),
          "request reply port write failed");
  require(client_memory.write32(client_buffer + 16, 0),
          "request voucher write failed");
  require(client_memory.write32(client_buffer + 20, 404),
          "request id write failed");
  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = 3; // MACH_SEND_MSG | MACH_RCV_MSG
  client_cpu.registers()[2] = 24;
  client_cpu.registers()[3] = 128;
  client_cpu.registers()[4] = reply_port;
  client.dispatch(client_cpu, 0x80);

  require(server.deliver_pending_mach(server_cpu),
          "bootstrap request did not wake server");
  const auto server_reply_name =
      server_memory.read32(server_buffer + 8).value_or(0);
  require(server_reply_name >= 0x10000 && server_reply_name != reply_port,
          "reply capability was not copied out under a server-local name");
  require(server_memory.read32(server_buffer + 12) ==
              std::optional<std::uint32_t>{server.process().bootstrap_port},
          "received request did not identify bootstrap destination");
  require(server_memory.read32(server_buffer + 20) ==
              std::optional<std::uint32_t>{404},
          "received bootstrap request id mismatch");

  require(server_memory.write32(server_buffer, 0x12),
          "reply bits write failed");
  require(server_memory.write32(server_buffer + 4, 36),
          "reply size write failed");
  require(server_memory.write32(server_buffer + 8, server_reply_name),
          "reply destination write failed");
  require(server_memory.write32(server_buffer + 12, 0),
          "reply local port write failed");
  require(server_memory.write32(server_buffer + 16, 0),
          "reply voucher write failed");
  require(server_memory.write32(server_buffer + 20, 504),
          "reply id write failed");
  require(server_memory.write32(server_buffer + 24, 0),
          "reply NDR write failed");
  require(server_memory.write32(server_buffer + 28, 1),
          "reply NDR endian write failed");
  require(server_memory.write32(server_buffer + 32, 1102),
          "reply code write failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 1; // MACH_SEND_MSG
  server_cpu.registers()[2] = 36;
  server_cpu.registers()[3] = 0;
  server_cpu.registers()[4] = 0;
  server.dispatch(server_cpu, 0x80);
  require(server_cpu.registers()[0] == 0, "server reply send failed");

  require(client.deliver_pending_mach(client_cpu), "reply did not wake client");
  require(client_memory.read32(client_buffer + 20) ==
              std::optional<std::uint32_t>{504},
          "received bootstrap reply id mismatch");
  require(client_memory.read32(client_buffer + 32) ==
              std::optional<std::uint32_t>{1102},
          "received bootstrap result mismatch");
}

void mach_port_lifecycle_notification_test() {
  AddressSpace server_memory;
  AddressSpace client_memory;
  constexpr std::uint32_t server_buffer = 0x51000;
  constexpr std::uint32_t client_buffer = 0x52000;
  require(
      server_memory.map(server_buffer, AddressSpace::page_size,
                        MemoryPermission::Read | MemoryPermission::Write) &&
          client_memory.map(client_buffer, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write),
      "Mach lifecycle notification buffers failed to map");
  Dynarmic::ExclusiveMonitor server_monitor{1};
  Dynarmic::ExclusiveMonitor client_monitor{1};
  Cpu server_cpu{0, server_memory, server_monitor};
  Cpu client_cpu{0, client_memory, client_monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel server{server_memory, output};
  CompatibilityKernel client{client_memory, output};
  client.inherit_process_state(server, 2);

  constexpr std::uint32_t client_task_name_address = server_buffer + 0x300U;
  server_cpu.registers()[0] = server.process().task_port;
  server_cpu.registers()[1] = client.process().pid;
  server_cpu.registers()[2] = client_task_name_address;
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-45);
  server.dispatch(server_cpu, 0x80);
  const auto client_task_name =
      server_memory.read32(client_task_name_address).value_or(0);
  require(server_cpu.registers()[0] == darwin::mach::success &&
              client_task_name != 0 &&
              client_task_name != client.process().task_port,
          "lifecycle server did not receive a local client task name");

  const auto call_mach = [](CompatibilityKernel &kernel, Cpu &cpu,
                            std::uint32_t buffer, std::uint32_t send_size,
                            std::uint32_t receive_size = 64U) {
    cpu.registers()[0] = buffer;
    cpu.registers()[1] = 3; // MACH_SEND_MSG | MACH_RCV_MSG
    cpu.registers()[2] = send_size;
    cpu.registers()[3] = receive_size;
    cpu.registers()[4] = 0x900;
    cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == 0, "Mach lifecycle RPC failed");
  };
  const auto allocate_receive = [&](CompatibilityKernel &kernel, Cpu &cpu,
                                    AddressSpace &memory,
                                    std::uint32_t buffer) {
    require(memory.write32(buffer, 0x1513) && memory.write32(buffer + 4, 36) &&
                memory.write32(buffer + 8, kernel.process().task_port) &&
                memory.write32(buffer + 12, 0x900) &&
                memory.write32(buffer + 20, 3204) &&
                memory.write32(buffer + 32, 1),
            "receive-right allocation request setup failed");
    call_mach(kernel, cpu, buffer, 36, 64);
    const auto name = memory.read32(buffer + 36).value_or(0);
    require(memory.read32(buffer + 32) == std::optional<std::uint32_t>{0} &&
                name != 0,
            "receive-right allocation failed");
    return name;
  };
  const auto destroy_name = [&](CompatibilityKernel &kernel, Cpu &cpu,
                                AddressSpace &memory, std::uint32_t buffer,
                                std::uint32_t name) {
    require(memory.write32(buffer, 0x1513) && memory.write32(buffer + 4, 36) &&
                memory.write32(buffer + 8, kernel.process().task_port) &&
                memory.write32(buffer + 12, 0x900) &&
                memory.write32(buffer + 20, 3205) &&
                memory.write32(buffer + 32, name),
            "mach_port_destroy request setup failed");
    call_mach(kernel, cpu, buffer, 36, 64);
    require(memory.read32(buffer + 32) == std::optional<std::uint32_t>{0},
            "mach_port_destroy returned an error");
  };
  const auto request_notification = [&call_mach](CompatibilityKernel &kernel,
                                                 Cpu &cpu, AddressSpace &memory,
                                                 std::uint32_t buffer,
                                                 std::uint32_t target,
                                                 std::uint32_t notification,
                                                 std::uint32_t notify,
                                                 std::uint32_t sync = 0) {
    require(
        memory.write32(buffer, 0x80001513U) && memory.write32(buffer + 4, 60) &&
            memory.write32(buffer + 8, kernel.process().task_port) &&
            memory.write32(buffer + 12, 0x900) &&
            memory.write32(buffer + 20, 3213) &&
            memory.write32(buffer + 24, 1) &&
            memory.write32(buffer + 28, notify) &&
            memory.write32(buffer + 36, 0x00150000U) &&
            memory.write32(buffer + 40, 0) && memory.write32(buffer + 44, 1) &&
            memory.write32(buffer + 48, target) &&
            memory.write32(buffer + 52, notification) &&
            memory.write32(buffer + 56, sync),
        "lifecycle notification request setup failed");
    call_mach(kernel, cpu, buffer, 60, 64);
    require(memory.read32(buffer + 4) == std::optional<std::uint32_t>{40} &&
                memory.read32(buffer + 28) == std::optional<std::uint32_t>{0},
            "lifecycle notification registration failed");
  };
  const auto receive_notification =
      [](CompatibilityKernel &kernel, Cpu &cpu, AddressSpace &memory,
         std::uint32_t buffer, std::uint32_t receive_name) {
        cpu.registers()[0] = buffer;
        cpu.registers()[1] = 2; // MACH_RCV_MSG
        cpu.registers()[2] = 0;
        cpu.registers()[3] = 64;
        cpu.registers()[4] = receive_name;
        cpu.registers()[12] = static_cast<std::uint32_t>(-31);
        kernel.dispatch(cpu, 0x80);
        require(cpu.registers()[0] == 0,
                "Mach lifecycle notification was not receivable");
        return memory.read32(buffer + 20).value_or(0);
      };
  const auto mod_refs = [&call_mach](CompatibilityKernel &kernel, Cpu &cpu,
                                     AddressSpace &memory, std::uint32_t buffer,
                                     std::uint32_t name, std::uint32_t right,
                                     std::int32_t delta) {
    require(memory.write32(buffer, 0x1513) && memory.write32(buffer + 4, 44) &&
                memory.write32(buffer + 8, kernel.process().task_port) &&
                memory.write32(buffer + 12, 0x900) &&
                memory.write32(buffer + 20, 3208) &&
                memory.write32(buffer + 32, name) &&
                memory.write32(buffer + 36, right) &&
                memory.write32(buffer + 40, static_cast<std::uint32_t>(delta)),
            "mach_port_mod_refs request setup failed");
    call_mach(kernel, cpu, buffer, 44, 64);
    return memory.read32(buffer + 32).value_or(0xffff'ffffU);
  };

  constexpr std::uint32_t allocated_name = 0x23000;
  constexpr std::uint32_t renamed_name = 0x23100;
  require(client_memory.write32(client_buffer, 0x1513) &&
              client_memory.write32(client_buffer + 4, 40) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x900) &&
              client_memory.write32(client_buffer + 20, 3203) &&
              client_memory.write32(client_buffer + 32, 1) &&
              client_memory.write32(client_buffer + 36, allocated_name),
          "mach_port_allocate_name request setup failed");
  call_mach(client, client_cpu, client_buffer, 40, 64);
  require(client_memory.read32(client_buffer + 32) ==
              std::optional<std::uint32_t>{0},
          "mach_port_allocate_name failed");

  require(client_memory.write32(client_buffer, 0x1513) &&
              client_memory.write32(client_buffer + 4, 40) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x900) &&
              client_memory.write32(client_buffer + 20, 3202) &&
              client_memory.write32(client_buffer + 32, allocated_name) &&
              client_memory.write32(client_buffer + 36, renamed_name),
          "mach_port_rename request setup failed");
  call_mach(client, client_cpu, client_buffer, 40, 64);
  require(client_memory.read32(client_buffer + 32) ==
              std::optional<std::uint32_t>{0},
          "mach_port_rename failed");

  require(client_memory.write32(client_buffer, 0x1513) &&
              client_memory.write32(client_buffer + 4, 40) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x900) &&
              client_memory.write32(client_buffer + 20, 3207) &&
              client_memory.write32(client_buffer + 32, renamed_name) &&
              client_memory.write32(client_buffer + 36, 1),
          "mach_port_get_refs request setup failed");
  call_mach(client, client_cpu, client_buffer, 40, 64);
  require(client_memory.read32(client_buffer + 32) ==
                  std::optional<std::uint32_t>{0} &&
              client_memory.read32(client_buffer + 36) ==
                  std::optional<std::uint32_t>{1},
          "mach_port_get_refs did not report one receive reference");

  require(client_memory.write32(client_buffer, 0x1513) &&
              client_memory.write32(client_buffer + 4, 24) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x900) &&
              client_memory.write32(client_buffer + 20, 3200),
          "mach_port_names request setup failed");
  call_mach(client, client_cpu, client_buffer, 24, 72);
  require(client_memory.read32(client_buffer + 4) ==
              std::optional<std::uint32_t>{68},
          "mach_port_names complex success reply size mismatch");
  const auto names_address =
      client_memory.read32(client_buffer + 28).value_or(0);
  const auto types_address =
      client_memory.read32(client_buffer + 40).value_or(0);
  const auto &names_arguments =
      xnu792::mig::mach_port::mach_port_names_arguments;
  const auto name_count =
      client_memory
          .read32(client_buffer + names_arguments[1].reply_count_offset)
          .value_or(0);
  require(client_memory.read32(client_buffer +
                               names_arguments[2].reply_count_offset) ==
              std::optional<std::uint32_t>{name_count},
          "mach_port_names returned mismatched names/types counts");
  bool found_renamed_receive = false;
  for (std::uint32_t index = 0; index < name_count; ++index) {
    if (client_memory.read32(names_address + index * 4U) ==
            std::optional<std::uint32_t>{renamed_name} &&
        client_memory.read32(types_address + index * 4U) ==
            std::optional<std::uint32_t>{0x00020000U}) {
      found_renamed_receive = true;
      break;
    }
  }
  require(found_renamed_receive,
          "mach_port_names omitted the renamed task-local receive right");
  destroy_name(client, client_cpu, client_memory, client_buffer, renamed_name);

  // A port-destroyed request backs up the receive right instead of killing
  // the object. The notification descriptor must copy it back under a new
  // name in the receiver's ipc_space.
  const auto backed_port =
      allocate_receive(client, client_cpu, client_memory, client_buffer);
  const auto backup_port =
      allocate_receive(client, client_cpu, client_memory, client_buffer);
  request_notification(client, client_cpu, client_memory, client_buffer,
                       backed_port, 69, backup_port);
  destroy_name(client, client_cpu, client_memory, client_buffer, backed_port);
  require(receive_notification(client, client_cpu, client_memory, client_buffer,
                               backup_port) == 69,
          "MACH_NOTIFY_PORT_DESTROYED message id mismatch");
  const auto recovered_receive =
      client_memory.read32(client_buffer + 28).value_or(0);
  // mach_port_destroy makes the old name immediately recyclable, so copyout
  // may legally choose either that value again or a different free name.
  require(recovered_receive != 0,
          "port-destroyed notification did not return a receive name");

  require(client_memory.write32(client_buffer, 0x1513) &&
              client_memory.write32(client_buffer + 4, 36) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x900) &&
              client_memory.write32(client_buffer + 20, 3201) &&
              client_memory.write32(client_buffer + 32, recovered_receive),
          "recovered receive type request setup failed");
  call_mach(client, client_cpu, client_buffer, 36, 64);
  require(client_memory.read32(client_buffer + 36) ==
              std::optional<std::uint32_t>{0x00020000U},
          "port-destroyed descriptor did not carry a receive right");

  // ipc_right_delta(RECEIVE, -1) preserves a same-name send right, then
  // port death converts it to a dead name. The generated DEAD_NAME
  // notification contributes one additional dead-name uref.
  const auto composite_port =
      allocate_receive(client, client_cpu, client_memory, client_buffer);
  require(client_memory.write32(client_buffer, 0x80001513U) &&
              client_memory.write32(client_buffer + 4, 52) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x900) &&
              client_memory.write32(client_buffer + 20, 3214) &&
              client_memory.write32(client_buffer + 24, 1) &&
              client_memory.write32(client_buffer + 28, composite_port) &&
              client_memory.write32(client_buffer + 36, 0x00140000U) &&
              client_memory.write32(client_buffer + 40, 0) &&
              client_memory.write32(client_buffer + 44, 1) &&
              client_memory.write32(client_buffer + 48, composite_port),
          "same-name send/receive insertion setup failed");
  call_mach(client, client_cpu, client_buffer, 52, 64);
  require(client_memory.read32(client_buffer + 32) ==
              std::optional<std::uint32_t>{0},
          "same-name send/receive insertion failed");
  const auto composite_notify =
      allocate_receive(client, client_cpu, client_memory, client_buffer);
  request_notification(client, client_cpu, client_memory, client_buffer,
                       composite_port, 72, composite_notify);
  require(mod_refs(client, client_cpu, client_memory, client_buffer,
                   composite_port, 1, -1) == 0,
          "composite receive-right mach_port_mod_refs failed");
  require(receive_notification(client, client_cpu, client_memory, client_buffer,
                               composite_notify) == 72 &&
              client_memory.read32(client_buffer + 32) ==
                  std::optional<std::uint32_t>{composite_port},
          "composite receive destruction omitted DEAD_NAME notification");
  require(client_memory.write32(client_buffer, 0x1513) &&
              client_memory.write32(client_buffer + 4, 40) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x900) &&
              client_memory.write32(client_buffer + 20, 3207) &&
              client_memory.write32(client_buffer + 32, composite_port) &&
              client_memory.write32(client_buffer + 36, 4),
          "composite dead-name get-refs setup failed");
  call_mach(client, client_cpu, client_buffer, 40, 64);
  require(client_memory.read32(client_buffer + 32) ==
                  std::optional<std::uint32_t>{0} &&
              client_memory.read32(client_buffer + 36) ==
                  std::optional<std::uint32_t>{2},
          "DEAD_NAME notification did not increment dead-name urefs");

  // A receive-only entry disappears entirely, so its dead-name request is
  // cancelled with PORT_DELETED rather than producing a dead name.
  const auto deleted_receive =
      allocate_receive(client, client_cpu, client_memory, client_buffer);
  const auto deleted_notify =
      allocate_receive(client, client_cpu, client_memory, client_buffer);
  request_notification(client, client_cpu, client_memory, client_buffer,
                       deleted_receive, 72, deleted_notify);
  require(mod_refs(client, client_cpu, client_memory, client_buffer,
                   deleted_receive, 1, -1) == 0,
          "receive-only mach_port_mod_refs failed");
  require(receive_notification(client, client_cpu, client_memory, client_buffer,
                               deleted_notify) == 65 &&
              client_memory.read32(client_buffer + 32) ==
                  std::optional<std::uint32_t>{deleted_receive},
          "released receive name omitted PORT_DELETED notification");

  // Port sets also have exactly one uref: zero delta is a no-op and -1
  // destroys the set and releases its task-local name.
  require(client_memory.write32(client_buffer, 0x1513) &&
              client_memory.write32(client_buffer + 4, 36) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x900) &&
              client_memory.write32(client_buffer + 20, 3204) &&
              client_memory.write32(client_buffer + 32, 3),
          "port-set allocation setup failed");
  call_mach(client, client_cpu, client_buffer, 36, 64);
  const auto port_set = client_memory.read32(client_buffer + 36).value_or(0);
  require(port_set != 0 &&
              mod_refs(client, client_cpu, client_memory, client_buffer,
                       port_set, 3, 0) == 0 &&
              mod_refs(client, client_cpu, client_memory, client_buffer,
                       port_set, 3, -1) == 0,
          "port-set mach_port_mod_refs semantics failed");
  require(client_memory.write32(client_buffer, 0x1513) &&
              client_memory.write32(client_buffer + 4, 36) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x900) &&
              client_memory.write32(client_buffer + 20, 3201) &&
              client_memory.write32(client_buffer + 32, port_set),
          "destroyed port-set type request setup failed");
  call_mach(client, client_cpu, client_buffer, 36, 64);
  require(client_memory.read32(client_buffer + 32) ==
              std::optional<std::uint32_t>{15},
          "destroyed port-set name remained in the ipc_space");

  // Install a send right to a server-owned receive object in the client's
  // namespace, then destroy the receive right and verify dead-name delivery.
  const auto server_receive =
      allocate_receive(server, server_cpu, server_memory, server_buffer);
  require(server_memory.write32(server_buffer, 0x80001513U) &&
              server_memory.write32(server_buffer + 4, 52) &&
              server_memory.write32(server_buffer + 8, client_task_name) &&
              server_memory.write32(server_buffer + 12, 0x900) &&
              server_memory.write32(server_buffer + 20, 3214) &&
              server_memory.write32(server_buffer + 24, 1) &&
              server_memory.write32(server_buffer + 28, server_receive) &&
              server_memory.write32(server_buffer + 36, 0x00140000U) &&
              server_memory.write32(server_buffer + 40, 0) &&
              server_memory.write32(server_buffer + 44, 1),
          "cross-task send-right insertion setup failed");
  constexpr std::uint32_t client_send_name = 0x22000;
  require(server_memory.write32(server_buffer + 48, client_send_name),
          "cross-task send name write failed");
  call_mach(server, server_cpu, server_buffer, 52, 64);
  require(server_memory.read32(server_buffer + 32) ==
              std::optional<std::uint32_t>{0},
          "cross-task send-right insertion failed");

  const auto mod_send_refs = [&](std::int32_t delta) {
    require(client_memory.write32(client_buffer, 0x1513) &&
                client_memory.write32(client_buffer + 4, 44) &&
                client_memory.write32(client_buffer + 8,
                                      client.process().task_port) &&
                client_memory.write32(client_buffer + 12, 0x900) &&
                client_memory.write32(client_buffer + 20, 3208) &&
                client_memory.write32(client_buffer + 32, client_send_name) &&
                client_memory.write32(client_buffer + 36, 0) &&
                client_memory.write32(client_buffer + 40,
                                      static_cast<std::uint32_t>(delta)),
            "mach_port_mod_refs request setup failed");
    call_mach(client, client_cpu, client_buffer, 44, 64);
    require(client_memory.read32(client_buffer + 32) ==
                std::optional<std::uint32_t>{0},
            "mach_port_mod_refs failed for a send right");
  };
  mod_send_refs(2);
  require(client_memory.write32(client_buffer, 0x1513) &&
              client_memory.write32(client_buffer + 4, 40) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x900) &&
              client_memory.write32(client_buffer + 20, 3207) &&
              client_memory.write32(client_buffer + 32, client_send_name) &&
              client_memory.write32(client_buffer + 36, 0),
          "send get-refs request setup failed");
  call_mach(client, client_cpu, client_buffer, 40, 64);
  require(client_memory.read32(client_buffer + 36) ==
              std::optional<std::uint32_t>{3},
          "positive mach_port_mod_refs did not update send urefs");
  mod_send_refs(-2);

  const auto dead_notify =
      allocate_receive(client, client_cpu, client_memory, client_buffer);
  request_notification(client, client_cpu, client_memory, client_buffer,
                       client_send_name, 72, dead_notify);
  destroy_name(server, server_cpu, server_memory, server_buffer,
               server_receive);
  require(receive_notification(client, client_cpu, client_memory, client_buffer,
                               dead_notify) == 72 &&
              client_memory.read32(client_buffer + 32) ==
                  std::optional<std::uint32_t>{client_send_name},
          "MACH_NOTIFY_DEAD_NAME payload mismatch");

  require(client_memory.write32(client_buffer, 0x1513) &&
              client_memory.write32(client_buffer + 4, 36) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x900) &&
              client_memory.write32(client_buffer + 20, 3201) &&
              client_memory.write32(client_buffer + 32, client_send_name),
          "dead-name type request setup failed");
  call_mach(client, client_cpu, client_buffer, 36, 64);
  require(client_memory.read32(client_buffer + 36) ==
              std::optional<std::uint32_t>{0x00100000U},
          "destroyed receive object did not convert send name to dead name");

  request_notification(client, client_cpu, client_memory, client_buffer,
                       client_send_name, 72, dead_notify, 1);
  require(receive_notification(client, client_cpu, client_memory, client_buffer,
                               dead_notify) == 72 &&
              client_memory.read32(client_buffer + 32) ==
                  std::optional<std::uint32_t>{client_send_name},
          "immediate dead-name notification was not delivered");
}

void mach_inflight_port_identity_test() {
  AddressSpace server_memory;
  AddressSpace client_memory;
  constexpr std::uint32_t server_buffer = 0x53000;
  constexpr std::uint32_t client_buffer = 0x54000;
  require(
      server_memory.map(server_buffer, AddressSpace::page_size,
                        MemoryPermission::Read | MemoryPermission::Write) &&
          client_memory.map(client_buffer, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write),
      "in-flight Mach buffers failed to map");
  Dynarmic::ExclusiveMonitor server_monitor{1};
  Dynarmic::ExclusiveMonitor client_monitor{1};
  Cpu server_cpu{0, server_memory, server_monitor};
  Cpu client_cpu{0, client_memory, client_monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel server{server_memory, output};
  CompatibilityKernel client{client_memory, output};
  client.inherit_process_state(server, 2);

  const auto rpc = [](CompatibilityKernel &kernel, Cpu &cpu,
                      std::uint32_t buffer, std::uint32_t send_size,
                      std::uint32_t receive_size = 64U) {
    cpu.registers()[0] = buffer;
    cpu.registers()[1] = 3;
    cpu.registers()[2] = send_size;
    cpu.registers()[3] = receive_size;
    cpu.registers()[4] = 0x950;
    cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == 0, "in-flight Mach RPC failed");
  };
  const auto port_type = [&](std::uint32_t name) {
    require(client_memory.write32(client_buffer, 0x1513) &&
                client_memory.write32(client_buffer + 4, 36) &&
                client_memory.write32(client_buffer + 8,
                                      client.process().task_port) &&
                client_memory.write32(client_buffer + 12, 0x950) &&
                client_memory.write32(client_buffer + 20, 3201) &&
                client_memory.write32(client_buffer + 32, name),
            "in-flight port-type setup failed");
    rpc(client, client_cpu, client_buffer, 36);
    return std::pair{
        client_memory.read32(client_buffer + 32).value_or(0xffff'ffffU),
        client_memory.read32(client_buffer + 36).value_or(0U)};
  };

  require(client_memory.write32(client_buffer, 0x1513) &&
              client_memory.write32(client_buffer + 4, 36) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x950) &&
              client_memory.write32(client_buffer + 20, 3204) &&
              client_memory.write32(client_buffer + 32, 1),
          "in-flight receive allocation setup failed");
  rpc(client, client_cpu, client_buffer, 36);
  const auto moved_name = client_memory.read32(client_buffer + 36).value_or(0);
  require(moved_name != 0, "in-flight receive allocation failed");

  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 2; // MACH_RCV_MSG
  server_cpu.registers()[2] = 0;
  server_cpu.registers()[3] = 64;
  server_cpu.registers()[4] = server.process().bootstrap_port;
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server.dispatch(server_cpu, 0x80);

  require(client_memory.write32(client_buffer, 0x80000013U) &&
              client_memory.write32(client_buffer + 4, 40) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().bootstrap_port) &&
              client_memory.write32(client_buffer + 12, 0) &&
              client_memory.write32(client_buffer + 20, 9010) &&
              client_memory.write32(client_buffer + 24, 1) &&
              client_memory.write32(client_buffer + 28, moved_name) &&
              client_memory.write32(client_buffer + 36, 0x00100000U),
          "MOVE_RECEIVE message setup failed");
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = 1;
  client_cpu.registers()[2] = 40;
  client_cpu.registers()[3] = 0;
  client_cpu.registers()[4] = 0;
  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0, "MOVE_RECEIVE message send failed");
  require(port_type(moved_name).first == 15,
          "MOVE_RECEIVE did not consume the sender name at send time");

  // Reuse the vacated numeric name for a different receive object before
  // the server takes the queued message.
  require(client_memory.write32(client_buffer, 0x1513) &&
              client_memory.write32(client_buffer + 4, 40) &&
              client_memory.write32(client_buffer + 8,
                                    client.process().task_port) &&
              client_memory.write32(client_buffer + 12, 0x950) &&
              client_memory.write32(client_buffer + 20, 3203) &&
              client_memory.write32(client_buffer + 32, 1) &&
              client_memory.write32(client_buffer + 36, moved_name),
          "in-flight name reuse setup failed");
  rpc(client, client_cpu, client_buffer, 40);
  require(client_memory.read32(client_buffer + 32) ==
              std::optional<std::uint32_t>{0},
          "in-flight sender name was not reusable");

  require(server.deliver_pending_mach(server_cpu) &&
              server_memory.read32(server_buffer + 20) ==
                  std::optional<std::uint32_t>{9010} &&
              server_memory.read32(server_buffer + 28).value_or(0) != 0,
          "queued MOVE_RECEIVE object was not delivered");
  require(port_type(moved_name) ==
              std::pair<std::uint32_t, std::uint32_t>{0, 0x00020000U},
          "delivery resolved the reused sender name instead of the in-flight "
          "object");
}

} // namespace

void run_ipc_lifecycle_tests() {
  cross_process_mach_ipc_test();
  mach_port_lifecycle_notification_test();
  mach_inflight_port_identity_test();
}

} // namespace ilemu::test::mach_suite
