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

void semaphore_wait_signal_test() {
  AddressSpace memory;
  constexpr std::uint32_t message = 0x36000;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "semaphore message map failed");
  Dynarmic::ExclusiveMonitor monitor{2};
  Cpu waiter_cpu{0, memory, monitor};
  Cpu signal_cpu{1, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  const auto create_semaphore = [&](std::uint32_t value) {
    require(memory.write32(message, 0x1513),
            "semaphore create bits write failed");
    require(memory.write32(message + 4, 40),
            "semaphore create size write failed");
    require(memory.write32(message + 8, kernel.process().task_port),
            "semaphore create task write failed");
    require(memory.write32(message + 12, 0x990),
            "semaphore create reply write failed");
    require(memory.write32(message + 20, 3418),
            "semaphore create id write failed");
    require(memory.write32(message + 32, 0),
            "semaphore create policy write failed");
    require(memory.write32(message + 36, value),
            "semaphore create value write failed");
    signal_cpu.registers()[0] = message;
    signal_cpu.registers()[1] = 3;
    signal_cpu.registers()[2] = 40;
    signal_cpu.registers()[3] = 40;
    signal_cpu.registers()[4] = 0;
    signal_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    kernel.dispatch(signal_cpu, 0x80);
    require(signal_cpu.registers()[0] == 0, "semaphore_create Mach RPC failed");
    const auto port = memory.read32(message + 28).value_or(0);
    require(port != 0, "semaphore_create returned a null port");
    return port;
  };

  const auto condition = create_semaphore(0);
  const auto mutex = create_semaphore(0);
  waiter_cpu.registers()[0] = condition;
  waiter_cpu.registers()[1] = mutex;
  waiter_cpu.registers()[2] = 0; // no timeout
  waiter_cpu.registers()[3] = 0;
  waiter_cpu.registers()[4] = 0;
  waiter_cpu.registers()[5] = 0;
  waiter_cpu.registers()[12] = 334;
  kernel.dispatch(waiter_cpu, 0x80);
  require(kernel.wait_reason(0).starts_with("semaphore("),
          "__semwait_signal did not block on the condition semaphore");

  signal_cpu.registers()[0] = condition;
  signal_cpu.registers()[12] = static_cast<std::uint32_t>(-33);
  kernel.dispatch(signal_cpu, 0x80);
  require(signal_cpu.registers()[0] == 0, "semaphore_signal_trap failed");
  require(kernel.deliver_pending_io(waiter_cpu),
          "semaphore signal did not wake the waiter");
  require(waiter_cpu.registers()[0] == 0,
          "__semwait_signal did not return success after wakeup");

  // The paired mutex signal must have been atomically preposted.
  signal_cpu.registers()[0] = mutex;
  signal_cpu.registers()[12] = static_cast<std::uint32_t>(-36);
  kernel.dispatch(signal_cpu, 0x80);
  require(signal_cpu.registers()[0] == 0 && kernel.wait_reason(1) == "none",
          "paired semaphore signal was not preposted");

  waiter_cpu.registers()[0] = condition;
  waiter_cpu.registers()[12] = static_cast<std::uint32_t>(-36);
  kernel.dispatch(waiter_cpu, 0x80);
  require(kernel.wait_reason(0).starts_with("semaphore("),
          "targeted-signal fixture did not block");

  signal_cpu.registers()[0] = condition;
  signal_cpu.registers()[1] = 0;
  signal_cpu.registers()[12] = static_cast<std::uint32_t>(-35);
  kernel.dispatch(signal_cpu, 0x80);
  require(signal_cpu.registers()[0] == 0 &&
              kernel.deliver_pending_io(waiter_cpu),
          "semaphore_signal_thread with a null target did not wake a waiter");

  signal_cpu.registers()[0] = condition;
  signal_cpu.registers()[1] = 0;
  signal_cpu.registers()[12] = static_cast<std::uint32_t>(-35);
  kernel.dispatch(signal_cpu, 0x80);
  require(signal_cpu.registers()[0] == 48,
          "semaphore_signal_thread preposted without a waiter");
}

void semaphore_task_namespace_test() {
  AddressSpace parent_memory;
  AddressSpace child_memory;
  constexpr std::uint32_t message = 0x36000;
  require(
      parent_memory.map(message, AddressSpace::page_size,
                        MemoryPermission::Read | MemoryPermission::Write) &&
          child_memory.map(message, AddressSpace::page_size,
                           MemoryPermission::Read | MemoryPermission::Write),
      "task-local semaphore message map failed");
  Dynarmic::ExclusiveMonitor parent_monitor{1};
  Dynarmic::ExclusiveMonitor child_monitor{1};
  Cpu parent_cpu{0, parent_memory, parent_monitor};
  Cpu child_cpu{0, child_memory, child_monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel parent{parent_memory, output};
  CompatibilityKernel child{child_memory, output};
  child.inherit_process_state(parent, 42);

  const auto create = [&](CompatibilityKernel &kernel, Cpu &cpu,
                          AddressSpace &memory, std::uint32_t value) {
    require(memory.write32(message, 0x1513) &&
                memory.write32(message + 4, 40) &&
                memory.write32(message + 8, kernel.process().task_port) &&
                memory.write32(message + 12, 0x990) &&
                memory.write32(message + 20, 3418) &&
                memory.write32(message + 32, 0) &&
                memory.write32(message + 36, value),
            "task-local semaphore create request write failed");
    cpu.registers()[0] = message;
    cpu.registers()[1] = 3;
    cpu.registers()[2] = 40;
    cpu.registers()[3] = 40;
    cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == 0, "task-local semaphore create failed");
    return memory.read32(message + 28).value_or(0);
  };
  const auto parent_name = create(parent, parent_cpu, parent_memory, 0);
  const auto child_name = create(child, child_cpu, child_memory, 1);
  require(parent_name != 0 && child_name != 0 && parent_name != child_name,
          "semaphore allocation ignored independent IPC-space cursors");

  child_cpu.registers()[0] = child_name;
  child_cpu.registers()[12] = static_cast<std::uint32_t>(-36);
  child.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] == 0 && child.wait_reason(0) == "none",
          "child semaphore did not consume its own prepost");

  parent_cpu.registers()[0] = parent_name;
  parent_cpu.registers()[12] = static_cast<std::uint32_t>(-36);
  parent.dispatch(parent_cpu, 0x80);
  require(parent.wait_reason(0).starts_with("semaphore("),
          "parent semaphore resolved to the child's IPC object");
  parent_cpu.registers()[0] = parent_name;
  parent_cpu.registers()[12] = static_cast<std::uint32_t>(-33);
  parent.dispatch(parent_cpu, 0x80);
  require(parent.deliver_pending_io(parent_cpu),
          "parent task-local semaphore did not wake");
}

} // namespace

void run_semaphore_tests() {
  semaphore_wait_signal_test();
  semaphore_task_namespace_test();
}

} // namespace ilemu::test::mach_suite
