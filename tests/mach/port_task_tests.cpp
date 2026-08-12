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

void mach_port_insert_move_right_test() {
  AddressSpace server_memory;
  AddressSpace client_memory;
  constexpr std::uint32_t server_buffer = 0x3e000;
  constexpr std::uint32_t client_buffer = 0x3f000;
  require(
      server_memory.map(server_buffer, AddressSpace::page_size,
                        MemoryPermission::Read | MemoryPermission::Write) &&
          client_memory.map(client_buffer, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write),
      "mach_port_insert_right buffers failed to map");
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
          "task_for_pid did not copy out a parent-local child task name");

  server_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  server.dispatch(server_cpu, 0x80);
  const auto receive_name = server_cpu.registers()[0];
  require(receive_name != 0, "receive right allocation failed");

  const auto insert = [&](std::uint32_t target_task, std::uint32_t source_name,
                          std::uint32_t disposition,
                          std::uint32_t target_name) {
    require(server_memory.write32(server_buffer, 0x80001513U) &&
                server_memory.write32(server_buffer + 4, 52) &&
                server_memory.write32(server_buffer + 8, target_task) &&
                server_memory.write32(server_buffer + 12, 0x900) &&
                server_memory.write32(server_buffer + 20, 3214) &&
                server_memory.write32(server_buffer + 24, 1) &&
                server_memory.write32(server_buffer + 28, source_name) &&
                server_memory.write32(server_buffer + 36, disposition << 16U) &&
                server_memory.write32(server_buffer + 40, 0) &&
                server_memory.write32(server_buffer + 44, 1) &&
                server_memory.write32(server_buffer + 48, target_name),
            "mach_port_insert_right request setup failed");
    server_cpu.registers()[0] = server_buffer;
    server_cpu.registers()[1] = 3;
    server_cpu.registers()[2] = 52;
    server_cpu.registers()[3] = 40;
    server_cpu.registers()[4] = 0x900;
    server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    server.dispatch(server_cpu, 0x80);
    return server_memory.read32(server_buffer + 32).value_or(0xffff'ffffU);
  };
  const auto port_type = [](CompatibilityKernel &kernel, Cpu &cpu,
                            AddressSpace &memory, std::uint32_t buffer,
                            std::uint32_t task, std::uint32_t name) {
    require(memory.write32(buffer, 0x1513) && memory.write32(buffer + 4, 36) &&
                memory.write32(buffer + 8, task) &&
                memory.write32(buffer + 12, 0x901) &&
                memory.write32(buffer + 20, 3201) &&
                memory.write32(buffer + 32, name),
            "mach_port_type request setup failed");
    cpu.registers()[0] = buffer;
    cpu.registers()[1] = 3;
    cpu.registers()[2] = 36;
    cpu.registers()[3] = 40;
    cpu.registers()[4] = 0x901;
    cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    kernel.dispatch(cpu, 0x80);
    require(memory.read32(buffer + 32) ==
                std::optional<std::uint32_t>{darwin::mach::success},
            "mach_port_type returned an error");
    return memory.read32(buffer + 36).value_or(0);
  };

  require(insert(server.process().task_port, receive_name, 20, receive_name) ==
              darwin::mach::success,
          "MAKE_SEND insert failed");
  constexpr std::uint32_t client_send_name = 0x27000;
  require(insert(client_task_name, receive_name, 17, client_send_name) ==
              darwin::mach::success,
          "MOVE_SEND insert into another task failed");
  require(port_type(server, server_cpu, server_memory, server_buffer,
                    server.process().task_port, receive_name) ==
                  xnu792::ipc::type_mask(xnu792::ipc::Right::Receive) &&
              port_type(client, client_cpu, client_memory, client_buffer,
                        client.process().task_port, client_send_name) ==
                  xnu792::ipc::type_mask(xnu792::ipc::Right::Send),
          "MOVE_SEND did not transfer only the selected right");

  constexpr std::uint32_t invalid_target_name = 0x27100;
  require(insert(client_task_name, receive_name, 19, invalid_target_name) !=
              darwin::mach::success,
          "COPY_SEND accepted a receive-only source name");

  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = 2;
  server_cpu.registers()[2] = 0;
  server_cpu.registers()[3] = 64;
  server_cpu.registers()[4] = receive_name;
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server.dispatch(server_cpu, 0x80);
  require(client_memory.write32(client_buffer, 0x13) &&
              client_memory.write32(client_buffer + 4, 24) &&
              client_memory.write32(client_buffer + 8, client_send_name) &&
              client_memory.write32(client_buffer + 12, 0) &&
              client_memory.write32(client_buffer + 16, 0) &&
              client_memory.write32(client_buffer + 20, 0x7711),
          "moved send-right message setup failed");
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = 1;
  client_cpu.registers()[2] = 24;
  client_cpu.registers()[3] = 0;
  client_cpu.registers()[4] = 0;
  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0 &&
              server.deliver_pending_mach(server_cpu) &&
              server_memory.read32(server_buffer + 20) ==
                  std::optional<std::uint32_t>{0x7711},
          "moved send right did not route to the retained receive right");
}

void task_special_port_test() {
  AddressSpace parent_memory;
  AddressSpace child_memory;
  constexpr std::uint32_t parent_message = 0x40000;
  constexpr std::uint32_t child_message = 0x50000;
  require(parent_memory.map(parent_message, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write),
          "parent task-special-port map failed");
  require(child_memory.map(child_message, AddressSpace::page_size,
                           MemoryPermission::Read | MemoryPermission::Write),
          "child task-special-port map failed");
  Dynarmic::ExclusiveMonitor parent_monitor{1};
  Dynarmic::ExclusiveMonitor child_monitor{1};
  Cpu parent_cpu{0, parent_memory, parent_monitor};
  Cpu child_cpu{0, child_memory, child_monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel parent{parent_memory, output};
  CompatibilityKernel child{child_memory, output};
  child.inherit_process_state(parent, 42);

  constexpr std::uint32_t child_task_name_address = parent_message + 0x300U;
  parent_cpu.registers()[0] = parent.process().task_port;
  parent_cpu.registers()[1] = child.process().pid;
  parent_cpu.registers()[2] = child_task_name_address;
  parent_cpu.registers()[12] = static_cast<std::uint32_t>(-45);
  parent.dispatch(parent_cpu, 0x80);
  const auto child_task_name =
      parent_memory.read32(child_task_name_address).value_or(0);
  require(parent_cpu.registers()[0] == darwin::mach::success &&
              child_task_name != 0 &&
              child_task_name != child.process().task_port,
          "parent did not receive a task-local child capability");

  // The parent already copied out a child-task capability, so its dynamic
  // name cursor is one slot ahead. Consume one child slot before forcing both
  // ipc_spaces to allocate the same local name for different receive objects.
  // The special-port transfer must preserve the parent's object identity, not
  // that colliding numeric name.
  child_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  child.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] != 0,
          "task-special collision cursor setup failed");
  child_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  child.dispatch(child_cpu, 0x80);
  const auto child_collision = child_cpu.registers()[0];
  parent_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  parent.dispatch(parent_cpu, 0x80);
  const auto parent_bootstrap = parent_cpu.registers()[0];
  require(parent_bootstrap == child_collision && parent_bootstrap != 0,
          "task-special collision setup did not reuse a local Mach name");

  // Form a SEND+RECEIVE entry so task_set_special_port can copy a send
  // capability to the child's TASK_BOOTSTRAP_PORT slot.
  require(parent_memory.write32(parent_message, 0x80001513U) &&
              parent_memory.write32(parent_message + 4, 52) &&
              parent_memory.write32(parent_message + 8,
                                    parent.process().task_port) &&
              parent_memory.write32(parent_message + 12, 0x900) &&
              parent_memory.write32(parent_message + 20, 3214) &&
              parent_memory.write32(parent_message + 24, 1) &&
              parent_memory.write32(parent_message + 28, parent_bootstrap) &&
              parent_memory.write32(parent_message + 36, 0x00140000U) &&
              parent_memory.write32(parent_message + 40, 0) &&
              parent_memory.write32(parent_message + 44, 1) &&
              parent_memory.write32(parent_message + 48, parent_bootstrap),
          "task-special MAKE_SEND setup failed");
  parent_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  parent_cpu.registers()[0] = parent_message;
  parent_cpu.registers()[1] = 3;
  parent_cpu.registers()[2] = 52;
  parent_cpu.registers()[3] = 40;
  parent_cpu.registers()[4] = 0x900;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_memory.read32(parent_message + 32) ==
              std::optional<std::uint32_t>{0},
          "task-special MAKE_SEND failed");

  require(parent_memory.write32(parent_message, 0x80001513),
          "task-set-special bits write failed");
  require(parent_memory.write32(parent_message + 4, 52),
          "task-set-special size write failed");
  require(parent_memory.write32(parent_message + 8, child_task_name),
          "task-set-special destination write failed");
  require(parent_memory.write32(parent_message + 12, 0x900),
          "task-set-special reply write failed");
  require(parent_memory.write32(parent_message + 20, 3410),
          "task-set-special id write failed");
  require(parent_memory.write32(parent_message + 24, 1),
          "task-set-special descriptor-count write failed");
  require(parent_memory.write32(parent_message + 28, parent_bootstrap),
          "task-set-special port write failed");
  require(parent_memory.write32(parent_message + 36, 0x00130000U),
          "task-set-special COPY_SEND disposition write failed");
  require(parent_memory.write32(parent_message + 40, 0) &&
              parent_memory.write32(parent_message + 44, 1),
          "task-set-special NDR write failed");
  require(parent_memory.write32(parent_message + 48, 4),
          "task-set-special selector write failed");
  parent_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  parent_cpu.registers()[0] = parent_message;
  parent_cpu.registers()[1] = 3;
  parent_cpu.registers()[2] = 52;
  parent_cpu.registers()[3] = 40;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == 0 &&
              parent_memory.read32(parent_message + 32) ==
                  std::optional<std::uint32_t>{0},
          "task_set_special_port failed");

  require(child_memory.write32(child_message, 0x1513),
          "task-get-special bits write failed");
  require(child_memory.write32(child_message + 8, child.process().task_port),
          "task-get-special destination write failed");
  require(child_memory.write32(child_message + 12, 0x901),
          "task-get-special reply write failed");
  require(child_memory.write32(child_message + 20, 3409),
          "task-get-special id write failed");
  require(child_memory.write32(child_message + 32, 4),
          "task-get-special selector write failed");
  child_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  child_cpu.registers()[0] = child_message;
  child_cpu.registers()[1] = 3;
  child_cpu.registers()[2] = 36;
  child_cpu.registers()[3] = 40;
  child.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] == 0, "task_get_special_port failed");
  const auto child_bootstrap_name =
      child_memory.read32(child_message + 28).value_or(0);
  require(child_bootstrap_name != 0,
          "child task received a null bootstrap special port");
  // A task special port stores an IPC object. Copyout into the child may
  // assign a different name than the caller used in the parent ipc_space.
  require(child.process().bootstrap_port == child_bootstrap_name,
          "task_get_special_port did not update the child bootstrap identity");
  require(child_bootstrap_name != child_collision,
          "bootstrap copyout aliased a different child-local port object");

  // Prove the copied-out name routes to the parent's receive object.
  parent_cpu.registers()[0] = parent_message;
  parent_cpu.registers()[1] = 2; // MACH_RCV_MSG
  parent_cpu.registers()[2] = 0;
  parent_cpu.registers()[3] = 64;
  parent_cpu.registers()[4] = parent_bootstrap;
  parent_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  parent.dispatch(parent_cpu, 0x80);
  require(child_memory.write32(child_message, 0x13) &&
              child_memory.write32(child_message + 4, 24) &&
              child_memory.write32(child_message + 8, child_bootstrap_name) &&
              child_memory.write32(child_message + 12, 0) &&
              child_memory.write32(child_message + 16, 0) &&
              child_memory.write32(child_message + 20, 777),
          "child bootstrap send setup failed");
  child_cpu.registers()[0] = child_message;
  child_cpu.registers()[1] = 1; // MACH_SEND_MSG
  child_cpu.registers()[2] = 24;
  child_cpu.registers()[3] = 0;
  child_cpu.registers()[4] = 0;
  child_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  child.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] == 0 &&
              parent.deliver_pending_mach(parent_cpu) &&
              parent_memory.read32(parent_message + 20) ==
                  std::optional<std::uint32_t>{777},
          "child bootstrap name did not preserve manager port identity");

  // A MOVE_SEND into the caller's own bootstrap slot consumes only the send
  // right. The receive half of the composite entry must survive, and a later
  // fork must inherit from the special-port object rather than from the now
  // invalid cached user name.
  require(parent_memory.write32(parent_message, 0x80001513U) &&
              parent_memory.write32(parent_message + 4, 52) &&
              parent_memory.write32(parent_message + 8,
                                    parent.process().task_port) &&
              parent_memory.write32(parent_message + 12, 0x900) &&
              parent_memory.write32(parent_message + 20, 3410) &&
              parent_memory.write32(parent_message + 24, 1) &&
              parent_memory.write32(parent_message + 28, parent_bootstrap) &&
              parent_memory.write32(parent_message + 36, 0x00110000U) &&
              parent_memory.write32(parent_message + 40, 0) &&
              parent_memory.write32(parent_message + 44, 1) &&
              parent_memory.write32(parent_message + 48, 4),
          "MOVE_SEND task-special setup failed");
  parent_cpu.registers()[0] = parent_message;
  parent_cpu.registers()[1] = 3;
  parent_cpu.registers()[2] = 52;
  parent_cpu.registers()[3] = 40;
  parent_cpu.registers()[4] = 0x900;
  parent_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == 0 &&
              parent_memory.read32(parent_message + 32) ==
                  std::optional<std::uint32_t>{0} &&
              parent.process().bootstrap_port == 0,
          "task_set_special_port did not consume MOVE_SEND precisely");

  AddressSpace second_child_memory;
  constexpr std::uint32_t second_child_message = 0x51000;
  require(
      second_child_memory.map(second_child_message, AddressSpace::page_size,
                              MemoryPermission::Read | MemoryPermission::Write),
      "second child task-special map failed");
  Dynarmic::ExclusiveMonitor second_child_monitor{1};
  Cpu second_child_cpu{0, second_child_memory, second_child_monitor};
  CompatibilityKernel second_child{second_child_memory, output};
  second_child.inherit_process_state(parent, 43);
  require(second_child.process().bootstrap_port != 0,
          "fork inherited bootstrap from a stale local name");

  parent_cpu.registers()[0] = parent_message;
  parent_cpu.registers()[1] = 2;
  parent_cpu.registers()[2] = 0;
  parent_cpu.registers()[3] = 64;
  parent_cpu.registers()[4] = parent_bootstrap;
  parent_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  parent.dispatch(parent_cpu, 0x80);
  require(
      second_child_memory.write32(second_child_message, 0x13) &&
          second_child_memory.write32(second_child_message + 4, 24) &&
          second_child_memory.write32(second_child_message + 8,
                                      second_child.process().bootstrap_port) &&
          second_child_memory.write32(second_child_message + 12, 0) &&
          second_child_memory.write32(second_child_message + 16, 0) &&
          second_child_memory.write32(second_child_message + 20, 778),
      "second child bootstrap send setup failed");
  second_child_cpu.registers()[0] = second_child_message;
  second_child_cpu.registers()[1] = 1;
  second_child_cpu.registers()[2] = 24;
  second_child_cpu.registers()[3] = 0;
  second_child_cpu.registers()[4] = 0;
  second_child_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  second_child.dispatch(second_child_cpu, 0x80);
  require(second_child_cpu.registers()[0] == 0 &&
              parent.deliver_pending_mach(parent_cpu) &&
              parent_memory.read32(parent_message + 20) ==
                  std::optional<std::uint32_t>{778},
          "MOVE_SEND destroyed the manager receive right or fork identity");
}

void task_pid_trap_test() {
  AddressSpace parent_memory;
  AddressSpace child_memory;
  constexpr std::uint32_t parent_output = 0x47000;
  constexpr std::uint32_t child_output = 0x48000;
  require(parent_memory.map(parent_output, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write),
          "parent task/pid output map failed");
  require(child_memory.map(child_output, AddressSpace::page_size,
                           MemoryPermission::Read | MemoryPermission::Write),
          "child task/pid output map failed");
  Dynarmic::ExclusiveMonitor parent_monitor{1};
  Dynarmic::ExclusiveMonitor child_monitor{1};
  Cpu parent_cpu{0, parent_memory, parent_monitor};
  Cpu child_cpu{0, child_memory, child_monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel parent{parent_memory, output};
  CompatibilityKernel child{child_memory, output};
  child.inherit_process_state(parent, 42);

  child_cpu.registers()[0] = child.process().task_port;
  child_cpu.registers()[1] = 1;
  child_cpu.registers()[2] = child_output;
  child_cpu.registers()[12] = static_cast<std::uint32_t>(-45);
  child.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] == 0,
          "task_for_pid did not return KERN_SUCCESS");
  const auto parent_task_name = child_memory.read32(child_output).value_or(0);
  require(parent_task_name != 0 &&
              parent_task_name != parent.process().task_port,
          "task_for_pid exposed the task object's global identifier");

  child_cpu.registers()[0] = parent_task_name;
  child_cpu.registers()[1] = child_output;
  child_cpu.registers()[12] = static_cast<std::uint32_t>(-46);
  child.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] == 0,
          "pid_for_task did not accept the copied-out task name");
  require(child_memory.read32(child_output) == std::optional<std::uint32_t>{1},
          "pid_for_task returned the wrong process id");

  child_cpu.registers()[0] = parent.process().task_port;
  child_cpu.registers()[1] = child_output;
  child_cpu.registers()[12] = static_cast<std::uint32_t>(-46);
  child.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] == 0 &&
              child_memory.read32(child_output) ==
                  std::optional<std::uint32_t>{child.process().pid},
          "colliding task_self name escaped the child's ipc_space");

  child_cpu.registers()[0] = 0xdeadbeefU;
  child_cpu.registers()[1] = child_output;
  child_cpu.registers()[12] = static_cast<std::uint32_t>(-46);
  child.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] == 5,
          "invalid pid_for_task did not return KERN_FAILURE");
  require(child_memory.read32(child_output) ==
              std::optional<std::uint32_t>{0xffffffffU},
          "invalid pid_for_task did not copy out -1");
}

void mach_message_abi_test() {
  std::vector<std::byte> bytes(24, std::byte{0});
  const auto put = [&](std::size_t offset, std::uint32_t value) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
      bytes[offset + byte] = static_cast<std::byte>(value >> (byte * 8U));
    }
  };
  const auto get = [](const std::vector<std::byte> &source,
                      std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < 4; ++byte) {
      value |= std::to_integer<std::uint32_t>(source[offset + byte])
               << (byte * 8U);
    }
    return value;
  };
  put(0, 0x1513);
  put(4, 0xdeadbeef);
  put(8, 0x111);
  put(12, 0x222);
  put(20, 402);
  require(mach_ipc::normalize_send_header(bytes, bytes.size()),
          "Mach send header normalization failed");
  require(get(bytes, 4) == 24, "Mach send size was not normalized");

  const KernelSharedState::MachMessage message{
      bytes, 0x111, 42, 501, 20,
  };
  const auto received = mach_ipc::prepare_received_message(
      message, 0x333,
      darwin::mig_wire::trailer_audit
          << darwin::mig_wire::trailer_elements_shift,
      7);
  require(received.has_value(), "Mach receive message preparation failed");
  require(received->bytes.size() == 76 && received->trailer_size == 52,
          "Darwin 8 audit trailer size mismatch");
  require(get(received->bytes, 0) == 0x1112,
          "Mach receive dispositions were not normalized and swapped");
  require(get(received->bytes, 8) == 0x222 && get(received->bytes, 12) == 0x333,
          "Mach receive header ports were not swapped");
  require(get(received->bytes, 28) == 52,
          "Mach trailer did not report its size");
  require(get(received->bytes, 32) == 7,
          "Mach trailer sequence number mismatch");
  require(get(received->bytes, 64) == 42,
          "Mach audit token sender PID mismatch");
}

void vm_map_anywhere_overlap_test() {
  AddressSpace memory;
  constexpr std::uint32_t message = 0x46000;
  constexpr std::uint32_t search_base = 0x10000000U;
  constexpr std::uint32_t occupied_page =
      search_base + 2U * AddressSpace::page_size;
  constexpr std::uint32_t allocation_size = 4U * AddressSpace::page_size;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "vm_map request page map failed");
  require(memory.map(occupied_page, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "vm_map obstacle page map failed");
  require(memory.write32(occupied_page, 0xfeedfaceU),
          "vm_map obstacle sentinel write failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  constexpr std::uint32_t request_size = 84U;
  require(memory.write32(
              message,
              darwin::mig_wire::message_bits(
                  darwin::mig_wire::disposition_copy_send,
                  darwin::mig_wire::disposition_make_send_once, true)),
          "vm_map bits write failed");
  require(memory.write32(message + 4, request_size),
          "vm_map size write failed");
  require(memory.write32(message + 8, kernel.process().task_port),
          "vm_map task write failed");
  require(memory.write32(message + 12, 0x990),
          "vm_map reply port write failed");
  require(memory.write32(message + 20, 3812), "vm_map id write failed");
  require(memory.write32(message + 24, 1),
          "vm_map descriptor count write failed");
  require(memory.write32(message + 28, 0),
          "vm_map memory object write failed");
  require(memory.write32(
              message + 36,
              darwin::mig_wire::port_descriptor_metadata(
                  darwin::mig_wire::disposition_copy_send)),
          "vm_map memory object descriptor write failed");
  require(memory.write32(message + 40, 0) &&
              memory.write32(message + 44, 1),
          "vm_map NDR write failed");
  require(memory.write32(message + 48, 0), "vm_map address write failed");
  require(memory.write32(message + 52, allocation_size),
          "vm_map allocation size write failed");
  require(memory.write32(message + 60, darwin::mach::vm_flags_anywhere),
          "vm_map flags write failed");
  require(memory.write32(message + 68, 0), "vm_map copy write failed");
  require(memory.write32(message + 72, 3) &&
              memory.write32(message + 76, 3),
          "vm_map protection write failed");
  require(memory.write32(message + 80, 0),
          "vm_map inheritance write failed");

  cpu.registers()[0] = message;
  cpu.registers()[1] = 3;
  cpu.registers()[2] = request_size;
  cpu.registers()[3] = 80;
  cpu.registers()[4] = 0;
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(cpu, 0x80);

  require(cpu.registers()[0] == 0, "vm_map Mach RPC failed");
  require(memory.read32(message + 32) == std::optional<std::uint32_t>{0},
          "vm_map returned a kernel error");
  const auto allocated = memory.read32(message + 36).value_or(0);
  require(allocated == search_base + 3U * AddressSpace::page_size,
          "vm_map anywhere did not search for a fully free range");
  require(memory.read32(occupied_page) ==
              std::optional<std::uint32_t>{0xfeedfaceU},
          "vm_map anywhere overwrote an occupied page");
}

} // namespace

void run_port_task_tests() {
  mach_port_insert_move_right_test();
  task_special_port_test();
  task_pid_trap_test();
  mach_message_abi_test();
  vm_map_anywhere_overlap_test();
}

} // namespace ilemu::test::mach_suite
