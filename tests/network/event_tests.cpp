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

void concurrent_kevent_wait_test() {
  AddressSpace memory;
  constexpr std::uint32_t base = 0x3d800;
  constexpr std::uint32_t pair_address = base;
  constexpr std::uint32_t change_address = base + 0x40;
  constexpr std::uint32_t first_event = base + 0x80;
  constexpr std::uint32_t second_event = base + 0xc0;
  constexpr std::uint32_t payload_address = base + 0x100;
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write) &&
              memory.write8(payload_address, static_cast<std::uint8_t>('k')),
          "concurrent kevent memory setup failed");
  Dynarmic::ExclusiveMonitor monitor{3};
  Cpu first_waiter{0, memory, monitor};
  Cpu second_waiter{1, memory, monitor};
  Cpu writer{2, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  writer.registers()[0] = darwin::socket::local;
  writer.registers()[1] = darwin::socket::stream;
  writer.registers()[2] = 0;
  writer.registers()[3] = pair_address;
  writer.registers()[12] = darwin::syscall::socket_pair;
  kernel.dispatch(writer, 0x80);
  const auto read_fd = memory.read32(pair_address).value_or(0);
  const auto write_fd = memory.read32(pair_address + 4).value_or(0);
  require(writer.registers()[0] == 0 && read_fd >= 3 && write_fd >= 3,
          "concurrent kevent socketpair creation failed");

  writer.registers()[12] = darwin::syscall::kqueue;
  kernel.dispatch(writer, 0x80);
  const auto queue_fd = writer.registers()[0];
  require(queue_fd > write_fd && memory.write32(change_address, read_fd) &&
              memory.write16(change_address + 4, 0xffffU) &&
              memory.write16(change_address + 6, 0x0001U) &&
              memory.write32(change_address + 8, 0) &&
              memory.write32(change_address + 12, 0) &&
              memory.write32(change_address + 16, 0x4b51U),
          "concurrent EVFILT_READ registration setup failed");
  writer.registers()[0] = queue_fd;
  writer.registers()[1] = change_address;
  writer.registers()[2] = 1;
  writer.registers()[3] = 0;
  writer.registers()[4] = 0;
  writer.registers()[5] = 0;
  writer.registers()[12] = darwin::syscall::kevent;
  kernel.dispatch(writer, 0x80);
  require(writer.registers()[0] == 0,
          "concurrent EVFILT_READ registration failed");

  const auto wait = [&](Cpu &cpu, std::uint32_t event_address) {
    cpu.registers()[0] = queue_fd;
    cpu.registers()[1] = 0;
    cpu.registers()[2] = 0;
    cpu.registers()[3] = event_address;
    cpu.registers()[4] = 1;
    cpu.registers()[5] = 0;
    cpu.registers()[12] = darwin::syscall::kevent;
    kernel.dispatch(cpu, 0x80);
  };
  wait(first_waiter, first_event);
  wait(second_waiter, second_event);
  const auto wait_description =
      "kevent(fd=" + std::to_string(queue_fd) + ",registrations=1)";
  require(kernel.wait_reason(0) == wait_description &&
              kernel.wait_reason(1) == wait_description,
          "a second kevent waiter replaced the first thread");

  writer.registers()[0] = write_fd;
  writer.registers()[1] = payload_address;
  writer.registers()[2] = 1;
  writer.registers()[12] = darwin::syscall::write;
  kernel.dispatch(writer, 0x80);
  require(writer.registers()[0] == 1 &&
              kernel.deliver_pending_io(first_waiter) &&
              kernel.wait_reason(1) == wait_description &&
              kernel.deliver_pending_io(second_waiter),
          "level-triggered readiness did not wake both kevent threads");
  require(
      first_waiter.registers()[0] == 1 && second_waiter.registers()[0] == 1 &&
          memory.read32(first_event) == std::optional<std::uint32_t>{read_fd} &&
          memory.read32(second_event) ==
              std::optional<std::uint32_t>{read_fd} &&
          memory.read32(first_event + 12) == std::optional<std::uint32_t>{1} &&
          memory.read32(second_event + 12) == std::optional<std::uint32_t>{1} &&
          memory.read32(first_event + 16) ==
              std::optional<std::uint32_t>{0x4b51U} &&
          memory.read32(second_event + 16) ==
              std::optional<std::uint32_t>{0x4b51U} &&
          kernel.wait_reason(0) == "none" && kernel.wait_reason(1) == "none",
      "concurrent kevent result payloads were not retained per thread");
}

void timed_kevent_wait_test() {
  AddressSpace memory;
  constexpr std::uint32_t base = 0x4e000;
  constexpr std::uint32_t pair_address = base;
  constexpr std::uint32_t change_address = base + 0x40;
  constexpr std::uint32_t event_address = base + 0x80;
  constexpr std::uint32_t timeout_address = base + 0xc0;
  constexpr std::uint32_t payload_address = base + 0x100;
  constexpr std::uint32_t timeout_nanoseconds = 25'000'000;
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write) &&
              memory.write8(payload_address, static_cast<std::uint8_t>('m')),
          "timed kevent memory map failed");

  Dynarmic::ExclusiveMonitor monitor{2};
  Cpu cpu{0, memory, monitor};
  Cpu writer{1, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  cpu.registers()[0] = darwin::socket::local;
  cpu.registers()[1] = darwin::socket::stream;
  cpu.registers()[2] = 0;
  cpu.registers()[3] = pair_address;
  cpu.registers()[12] = darwin::syscall::socket_pair;
  kernel.dispatch(cpu, 0x80);
  const auto read_fd = memory.read32(pair_address).value_or(0);
  const auto write_fd = memory.read32(pair_address + 4).value_or(0);
  require(cpu.registers()[0] == 0 && read_fd >= 3 && write_fd >= 3,
          "timed kevent socketpair failed");

  cpu.registers()[12] = darwin::syscall::kqueue;
  kernel.dispatch(cpu, 0x80);
  const auto queue_fd = cpu.registers()[0];
  require(
      queue_fd >= 3 &&
          memory.write32(change_address +
                             darwin::kqueue::arm32_event::identifier_offset,
                         read_fd) &&
          memory.write16(
              change_address + darwin::kqueue::arm32_event::filter_offset,
              static_cast<std::uint16_t>(darwin::kqueue::filter_read)) &&
          memory.write16(change_address +
                             darwin::kqueue::arm32_event::flags_offset,
                         darwin::kqueue::event_add) &&
          memory.write32(change_address +
                             darwin::kqueue::arm32_event::filter_flags_offset,
                         0) &&
          memory.write32(
              change_address + darwin::kqueue::arm32_event::data_offset, 0) &&
          memory.write32(change_address +
                             darwin::kqueue::arm32_event::user_data_offset,
                         0x4d444e53U),
      "timed EVFILT_READ fixture setup failed");
  cpu.registers()[0] = queue_fd;
  cpu.registers()[1] = change_address;
  cpu.registers()[2] = 1;
  cpu.registers()[3] = 0;
  cpu.registers()[4] = 0;
  cpu.registers()[5] = 0;
  cpu.registers()[12] = darwin::syscall::kevent;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "timed EVFILT_READ registration failed");

  require(
      memory.write32(timeout_address +
                         darwin::kqueue::arm32_timespec::seconds_offset,
                     0) &&
          memory.write32(timeout_address +
                             darwin::kqueue::arm32_timespec::nanoseconds_offset,
                         timeout_nanoseconds),
      "timed kevent timespec setup failed");
  cpu.registers()[0] = queue_fd;
  cpu.registers()[1] = 0;
  cpu.registers()[2] = 0;
  cpu.registers()[3] = event_address;
  cpu.registers()[4] = 1;
  cpu.registers()[5] = timeout_address;
  cpu.registers()[12] = darwin::syscall::kevent;
  kernel.dispatch(cpu, 0x80);
  const auto deadline = kernel.next_timer_deadline();
  const auto wait_description = kernel.wait_reason(cpu.processor_id());
  const auto delivered_early = kernel.deliver_pending_io(cpu);
  if (cpu.registers()[0] != 0 || !deadline ||
      *deadline != VirtualClock::default_initial_time + timeout_nanoseconds ||
      wait_description !=
          "kevent(fd=" + std::to_string(queue_fd) + ",registrations=1)" ||
      delivered_early) {
    throw std::runtime_error{"finite kevent suspension mismatch: result=" +
                             std::to_string(cpu.registers()[0]) + " deadline=" +
                             (deadline ? std::to_string(*deadline) : "none") +
                             " wait=" + wait_description +
                             " early=" + std::to_string(delivered_early)};
  }

  kernel.advance_absolute_time(*deadline - 1U);
  require(!kernel.deliver_pending_io(cpu),
          "finite kevent woke before its deadline");
  kernel.advance_absolute_time(*deadline);
  require(kernel.deliver_pending_io(cpu) && cpu.registers()[0] == 0 &&
              kernel.wait_reason(cpu.processor_id()) == "none" &&
              !kernel.next_timer_deadline(),
          "finite kevent did not return zero at timeout");

  require(memory.write32(timeout_address +
                             darwin::kqueue::arm32_timespec::nanoseconds_offset,
                         darwin::kqueue::nanoseconds_per_second),
          "invalid kevent timespec setup failed");
  cpu.registers()[0] = queue_fd;
  cpu.registers()[1] = 0;
  cpu.registers()[2] = 0;
  cpu.registers()[3] = event_address;
  cpu.registers()[4] = 1;
  cpu.registers()[5] = timeout_address;
  cpu.registers()[12] = darwin::syscall::kevent;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == darwin::error::invalid_argument &&
              kernel.wait_reason(cpu.processor_id()) == "none",
          "kevent accepted a non-normalized timespec");

  constexpr std::uint32_t readiness_timeout = 100'000'000;
  require(memory.write32(timeout_address +
                             darwin::kqueue::arm32_timespec::nanoseconds_offset,
                         readiness_timeout),
          "ready-before-timeout timespec setup failed");
  cpu.registers()[0] = queue_fd;
  cpu.registers()[1] = 0;
  cpu.registers()[2] = 0;
  cpu.registers()[3] = event_address;
  cpu.registers()[4] = 1;
  cpu.registers()[5] = timeout_address;
  cpu.registers()[12] = darwin::syscall::kevent;
  kernel.dispatch(cpu, 0x80);
  require(kernel.next_timer_deadline().has_value(),
          "ready-before-timeout kevent did not retain its deadline");

  writer.registers()[0] = write_fd;
  writer.registers()[1] = payload_address;
  writer.registers()[2] = 1;
  writer.registers()[12] = darwin::syscall::write;
  kernel.dispatch(writer, 0x80);
  require(writer.registers()[0] == 1 && kernel.deliver_pending_io(cpu) &&
              cpu.registers()[0] == 1 &&
              memory.read32(event_address +
                            darwin::kqueue::arm32_event::identifier_offset) ==
                  std::optional<std::uint32_t>{read_fd} &&
              memory.read32(event_address +
                            darwin::kqueue::arm32_event::user_data_offset) ==
                  std::optional<std::uint32_t>{0x4d444e53U} &&
              !kernel.next_timer_deadline(),
          "descriptor readiness did not preempt finite kevent timeout");
}

void kqueue_descriptor_close_detach_test() {
  AddressSpace memory;
  constexpr std::uint32_t base = 0x4f000;
  constexpr std::uint32_t first_pair = base;
  constexpr std::uint32_t second_pair = base + 0x20;
  constexpr std::uint32_t change = base + 0x40;
  constexpr std::uint32_t event = base + 0x80;
  constexpr std::uint32_t timeout = base + 0xc0;
  constexpr std::uint32_t payload = base + 0x100;
  constexpr std::uint32_t stale_user_data = 0xdead'4b51U;
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write) &&
              memory.write8(payload, static_cast<std::uint8_t>('k')),
          "kqueue close-detach memory setup failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  const auto make_pair = [&](std::uint32_t address) {
    cpu.registers()[0] = darwin::socket::local;
    cpu.registers()[1] = darwin::socket::stream;
    cpu.registers()[2] = 0;
    cpu.registers()[3] = address;
    cpu.registers()[12] = darwin::syscall::socket_pair;
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == 0, "kqueue close-detach socketpair failed");
    return std::pair{
        memory.read32(address).value_or(0),
        memory.read32(address + sizeof(std::uint32_t)).value_or(0)};
  };
  const auto [old_read, old_write] = make_pair(first_pair);
  cpu.registers()[12] = darwin::syscall::kqueue;
  kernel.dispatch(cpu, 0x80);
  const auto queue_fd = cpu.registers()[0];
  require(
      old_read >= 3 && old_write >= 3 && queue_fd >= 3 &&
          memory.write32(change +
                             darwin::kqueue::arm32_event::identifier_offset,
                         old_read) &&
          memory.write16(
              change + darwin::kqueue::arm32_event::filter_offset,
              static_cast<std::uint16_t>(darwin::kqueue::filter_read)) &&
          memory.write16(change + darwin::kqueue::arm32_event::flags_offset,
                         darwin::kqueue::event_add) &&
          memory.write32(
              change + darwin::kqueue::arm32_event::filter_flags_offset, 0) &&
          memory.write32(change + darwin::kqueue::arm32_event::data_offset,
                         0) &&
          memory.write32(change + darwin::kqueue::arm32_event::user_data_offset,
                         stale_user_data),
      "kqueue close-detach registration setup failed");
  cpu.registers()[0] = queue_fd;
  cpu.registers()[1] = change;
  cpu.registers()[2] = 1;
  cpu.registers()[3] = 0;
  cpu.registers()[4] = 0;
  cpu.registers()[5] = 0;
  cpu.registers()[12] = darwin::syscall::kevent;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "kqueue close-detach registration failed");

  cpu.registers()[0] = old_read;
  cpu.registers()[12] = darwin::syscall::close;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "kqueue watched descriptor close failed");
  const auto [new_read, new_write] = make_pair(second_pair);
  require(new_read == old_read,
          "kqueue close-detach fixture did not reuse the watched fd");

  cpu.registers()[0] = new_write;
  cpu.registers()[1] = payload;
  cpu.registers()[2] = 1;
  cpu.registers()[12] = darwin::syscall::write;
  kernel.dispatch(cpu, 0x80);
  require(
      cpu.registers()[0] == 1 &&
          memory.write32(event + darwin::kqueue::arm32_event::user_data_offset,
                         stale_user_data) &&
          memory.write32(
              timeout + darwin::kqueue::arm32_timespec::seconds_offset, 0) &&
          memory.write32(
              timeout + darwin::kqueue::arm32_timespec::nanoseconds_offset, 0),
      "kqueue close-detach readiness setup failed");
  cpu.registers()[0] = queue_fd;
  cpu.registers()[1] = 0;
  cpu.registers()[2] = 0;
  cpu.registers()[3] = event;
  cpu.registers()[4] = 1;
  cpu.registers()[5] = timeout;
  cpu.registers()[12] = darwin::syscall::kevent;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              memory.read32(event +
                            darwin::kqueue::arm32_event::user_data_offset) ==
                  std::optional<std::uint32_t>{stale_user_data},
          "closed descriptor knote survived fd reuse");
}

} // namespace

void run_event_tests() {
  concurrent_kevent_wait_test();
  timed_kevent_wait_test();
  kqueue_descriptor_close_detach_test();
}

} // namespace ilemu::test::network_suite
