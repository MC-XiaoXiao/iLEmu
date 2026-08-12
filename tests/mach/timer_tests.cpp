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
#include "ilemu/mach_host_mig_ids.hpp"
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

void mach_timer_wait_test() {
  AddressSpace memory;
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};
  constexpr std::uint64_t deadline = 5'000'000;
  cpu.registers()[0] = static_cast<std::uint32_t>(deadline);
  cpu.registers()[1] = static_cast<std::uint32_t>(deadline >> 32U);
  cpu.registers()[12] = static_cast<std::uint32_t>(-90);
  kernel.dispatch(cpu, 0x80);
  require(kernel.next_timer_deadline() ==
              std::optional<std::uint64_t>{deadline},
          "mach_wait_until did not enqueue a timer");
  require(!kernel.deliver_pending_io(cpu),
          "mach_wait_until completed before its deadline");
  kernel.advance_absolute_time(deadline);
  require(kernel.deliver_pending_io(cpu),
          "mach_wait_until was not delivered at its deadline");
  require(!kernel.next_timer_deadline(),
          "delivered Mach timer remained queued");
  require(cpu.registers()[0] == 0, "mach_wait_until did not return success");
}

void mach_thread_switch_test() {
  AddressSpace memory;
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  cpu.registers()[0] = 0;
  cpu.registers()[1] = darwin::mach::scheduler::switch_option_depress;
  cpu.registers()[2] = 1;
  cpu.registers()[12] = static_cast<std::uint32_t>(-61);
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == darwin::mach::success,
          "thread_switch DEPRESS did not return success");
  const auto yielded = cpu.run(1);
  require(Dynarmic::Has(yielded.reason, Dynarmic::HaltReason::UserDefined8),
          "thread_switch DEPRESS did not yield its time slice");

  cpu.clear_halt();
  cpu.registers()[0] = 0;
  cpu.registers()[1] = darwin::mach::scheduler::switch_option_wait;
  cpu.registers()[2] = 2;
  cpu.registers()[12] = static_cast<std::uint32_t>(-61);
  kernel.dispatch(cpu, 0x80);
  const auto deadline = kernel.next_timer_deadline();
  require(deadline &&
              *deadline ==
                  VirtualClock::default_initial_time +
                      2 * darwin::mach::scheduler::nanoseconds_per_millisecond,
          "thread_switch WAIT did not enqueue its millisecond timeout");
  require(!kernel.deliver_pending_io(cpu),
          "thread_switch WAIT completed before its deadline");
  kernel.advance_absolute_time(*deadline);
  require(kernel.deliver_pending_io(cpu),
          "thread_switch WAIT was not delivered at its deadline");
  require(cpu.registers()[0] == darwin::mach::success,
          "thread_switch WAIT did not return success");

  cpu.registers()[1] = darwin::mach::scheduler::maximum_switch_option + 1;
  cpu.registers()[12] = static_cast<std::uint32_t>(-61);
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == darwin::mach::invalid_argument,
          "thread_switch accepted an invalid option");
}

void mach_clock_sleep_test() {
  AddressSpace memory;
  constexpr std::uint32_t wakeup_time = 0x34000;
  require(memory.map(wakeup_time, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "clock_sleep wakeup-time map failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  constexpr std::uint32_t interval_nanoseconds = 2'000'000;
  cpu.registers()[0] = kernel.process().clock_port;
  cpu.registers()[1] = darwin::mach::clock::time_relative;
  cpu.registers()[2] = 0;
  cpu.registers()[3] = interval_nanoseconds;
  cpu.registers()[4] = wakeup_time;
  cpu.registers()[12] = static_cast<std::uint32_t>(
      -static_cast<std::int32_t>(darwin::mach::clock::sleep_trap));
  kernel.dispatch(cpu, 0x80);

  const auto expected_deadline =
      VirtualClock::default_initial_time + interval_nanoseconds;
  require(kernel.next_timer_deadline() ==
              std::optional<std::uint64_t>{expected_deadline},
          "relative clock_sleep did not enqueue an absolute deadline");
  require(!kernel.deliver_pending_io(cpu),
          "clock_sleep completed before its deadline");
  kernel.advance_absolute_time(expected_deadline);
  require(kernel.deliver_pending_io(cpu),
          "clock_sleep was not delivered at its deadline");
  require(cpu.registers()[0] == darwin::mach::success,
          "clock_sleep did not return success");
  require(memory.read32(wakeup_time) == std::optional<std::uint32_t>{0} &&
              memory.read32(wakeup_time + sizeof(std::uint32_t)) ==
                  std::optional<std::uint32_t>{
                      static_cast<std::uint32_t>(expected_deadline)},
          "clock_sleep did not copy out the virtual wakeup time");

  cpu.registers()[0] = 0;
  cpu.registers()[1] = darwin::mach::clock::time_absolute;
  cpu.registers()[2] = 0;
  cpu.registers()[3] = 0;
  cpu.registers()[4] = wakeup_time;
  cpu.registers()[12] = static_cast<std::uint32_t>(
      -static_cast<std::int32_t>(darwin::mach::clock::sleep_trap));
  kernel.dispatch(cpu, 0x80);
  require(!kernel.next_timer_deadline() &&
              cpu.registers()[0] == darwin::mach::success,
          "past absolute clock_sleep did not complete immediately");
  require(memory.read32(wakeup_time + sizeof(std::uint32_t)) ==
              std::optional<std::uint32_t>{
                  static_cast<std::uint32_t>(expected_deadline)},
          "immediate clock_sleep copied out the wrong current time");

  cpu.registers()[0] = 0;
  cpu.registers()[1] = darwin::mach::clock::time_relative;
  cpu.registers()[2] = 0;
  cpu.registers()[3] =
      static_cast<std::uint32_t>(darwin::mach::clock::nanoseconds_per_second);
  cpu.registers()[12] = static_cast<std::uint32_t>(
      -static_cast<std::int32_t>(darwin::mach::clock::sleep_trap));
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == darwin::mach::invalid_value,
          "clock_sleep accepted a non-normalized timespec");
}

void mach_clock_mig_test() {
  namespace clock_mig = xnu792::mig::clock;
  AddressSpace memory;
  constexpr std::uint32_t message = 0x35000;
  constexpr std::uint32_t reply_port = 0x901;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "clock MIG message map failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  const auto write_header = [&](std::uint32_t identifier,
                                std::uint32_t send_size) {
    return memory.write32(message + darwin::mig_wire::header_bits_offset,
                          0x1513U) &&
           memory.write32(message + darwin::mig_wire::header_size_offset,
                          send_size) &&
           memory.write32(message + darwin::mig_wire::header_remote_port_offset,
                          kernel.process().clock_port) &&
           memory.write32(message + darwin::mig_wire::header_local_port_offset,
                          reply_port) &&
           memory.write32(message + darwin::mig_wire::header_identifier_offset,
                          identifier);
  };
  const auto call = [&](std::uint32_t send_size) {
    cpu.registers()[0] = message;
    cpu.registers()[1] = 3;
    cpu.registers()[2] = send_size;
    cpu.registers()[3] = 64;
    cpu.registers()[4] = reply_port;
    cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == 0, "clock MIG RPC failed");
  };

  constexpr auto time_send_size = darwin::mig_wire::message_header_size;
  require(write_header(clock_mig::id(clock_mig::Routine::clock_get_time),
                       time_send_size),
          "clock_get_time request setup failed");
  call(time_send_size);
  constexpr const auto &time_output = clock_mig::clock_get_time_arguments[1];
  require(memory.read32(message + darwin::mig_wire::header_size_offset) ==
                  std::optional<std::uint32_t>{time_output.reply_offset +
                                               time_output.wire_size} &&
              memory.read32(message +
                            darwin::mig_wire::simple_request_payload_base) ==
                  std::optional<std::uint32_t>{darwin::mach::success} &&
              memory.read32(message + time_output.reply_offset) ==
                  std::optional<std::uint32_t>{0} &&
              memory.read32(message + time_output.reply_offset +
                            sizeof(std::uint32_t)) ==
                  std::optional<std::uint32_t>{static_cast<std::uint32_t>(
                      VirtualClock::default_initial_time)},
          "clock_get_time returned the wrong virtual mach_timespec");

  constexpr std::uint64_t calendar_seconds = 1'800'000'000ULL;
  constexpr std::uint64_t calendar_nanoseconds = 456'789'000ULL;
  kernel.synchronize_wall_time(
      calendar_seconds * VirtualClock::nanoseconds_per_second +
      calendar_nanoseconds);

  namespace host_mig = xnu792::mig::mach_host;
  constexpr const auto &clock_service_arguments =
      host_mig::host_get_clock_service_arguments;
  constexpr auto clock_service_send_size =
      clock_service_arguments[1].request_offset +
      clock_service_arguments[1].wire_size;
  require(memory.write32(message + darwin::mig_wire::header_bits_offset,
                         0x1513U) &&
              memory.write32(message + darwin::mig_wire::header_size_offset,
                             clock_service_send_size) &&
              memory.write32(
                  message + darwin::mig_wire::header_remote_port_offset,
                  kernel.process().host_port) &&
              memory.write32(
                  message + darwin::mig_wire::header_local_port_offset,
                  reply_port) &&
              memory.write32(message +
                                 darwin::mig_wire::header_identifier_offset,
                             host_mig::id(
                                 host_mig::Routine::host_get_clock_service)) &&
              memory.write32(message +
                                 clock_service_arguments[1].request_offset,
                             darwin::mach::clock::calendar_clock_id),
          "calendar host_get_clock_service request setup failed");
  call(clock_service_send_size);
  const auto calendar_clock = memory.read32(
      message + clock_service_arguments[2].reply_offset);
  require(calendar_clock && *calendar_clock != 0,
          "host_get_clock_service returned no calendar clock port");

  require(write_header(clock_mig::id(clock_mig::Routine::clock_get_time),
                       time_send_size) &&
              memory.write32(
                  message + darwin::mig_wire::header_remote_port_offset,
                  *calendar_clock),
          "calendar clock_get_time request setup failed");
  call(time_send_size);
  require(memory.read32(message + time_output.reply_offset) ==
                  std::optional<std::uint32_t>{calendar_seconds} &&
              memory.read32(message + time_output.reply_offset +
                            sizeof(std::uint32_t)) ==
                  std::optional<std::uint32_t>{calendar_nanoseconds},
          "calendar clock_get_time did not return synchronized wall time");

  kernel.advance_time_by(2 * VirtualClock::nanoseconds_per_second);
  require(write_header(clock_mig::id(clock_mig::Routine::clock_get_time),
                       time_send_size) &&
              memory.write32(
                  message + darwin::mig_wire::header_remote_port_offset,
                  *calendar_clock),
          "advanced calendar clock_get_time request setup failed");
  call(time_send_size);
  require(memory.read32(message + time_output.reply_offset) ==
                  std::optional<std::uint32_t>{calendar_seconds + 2} &&
              memory.read32(message + time_output.reply_offset +
                            sizeof(std::uint32_t)) ==
                  std::optional<std::uint32_t>{calendar_nanoseconds},
          "calendar clock_get_time did not advance");

  constexpr const auto &attribute_arguments =
      clock_mig::clock_get_attributes_arguments;
  constexpr auto attribute_send_size =
      attribute_arguments[2].request_count_offset + sizeof(std::uint32_t);
  const auto call_attributes = [&](std::uint32_t flavor, std::uint32_t count) {
    require(
        write_header(clock_mig::id(clock_mig::Routine::clock_get_attributes),
                     attribute_send_size) &&
            memory.write32(message + attribute_arguments[1].request_offset,
                           flavor) &&
            memory.write32(
                message + attribute_arguments[2].request_count_offset, count),
        "clock_get_attributes request setup failed");
    call(attribute_send_size);
  };
  call_attributes(darwin::mach::clock::get_time_resolution_flavor,
                  darwin::mach::clock::attribute_word_count);
  require(memory.read32(message + attribute_arguments[2].reply_count_offset) ==
                  std::optional<std::uint32_t>{1} &&
              memory.read32(message + attribute_arguments[2].reply_offset) ==
                  std::optional<std::uint32_t>{
                      darwin::mach::clock::virtual_resolution_nanoseconds},
          "clock_get_attributes returned the wrong time resolution");

  call_attributes(darwin::mach::clock::get_time_resolution_flavor, 2);
  require(
      memory.read32(message + darwin::mig_wire::simple_request_payload_base) ==
          std::optional<std::uint32_t>{darwin::mach::failure},
      "clock_get_attributes accepted a non-XNU attribute count");
  call_attributes(0xffffU, darwin::mach::clock::attribute_word_count);
  require(
      memory.read32(message + darwin::mig_wire::simple_request_payload_base) ==
          std::optional<std::uint32_t>{darwin::mach::invalid_value},
      "clock_get_attributes accepted an unknown flavor");

  constexpr const auto &alarm_arguments = clock_mig::clock_alarm_arguments;
  constexpr auto alarm_send_size =
      alarm_arguments[2].request_offset + alarm_arguments[2].wire_size;
  constexpr std::uint32_t alarm_interval = 2'000'000;
  const auto alarm_deadline =
      kernel.current_absolute_time() + alarm_interval;
  require(
      write_header(clock_mig::id(clock_mig::Routine::clock_alarm),
                   alarm_send_size) &&
          memory.write32(message + darwin::mig_wire::header_bits_offset,
                         0x80001513U) &&
          memory.write32(
              message + darwin::mig_wire::complex_descriptor_count_offset, 1) &&
          memory.write32(message + alarm_arguments[3].request_offset,
                         kernel.process().bootstrap_port) &&
          memory.write32(message + alarm_arguments[3].request_offset +
                             2U * sizeof(std::uint32_t),
                         0x00150000U) &&
          memory.write32(message + alarm_arguments[1].request_offset,
                         darwin::mach::clock::time_relative) &&
          memory.write32(message + alarm_arguments[2].request_offset, 0) &&
          memory.write32(message + alarm_arguments[2].request_offset +
                             sizeof(std::uint32_t),
                         alarm_interval),
      "clock_alarm request setup failed");
  call(alarm_send_size);
  require(
      memory.read32(message + darwin::mig_wire::simple_request_payload_base) ==
              std::optional<std::uint32_t>{darwin::mach::success} &&
          kernel.next_timer_deadline() ==
              std::optional<std::uint64_t>{alarm_deadline},
      "clock_alarm did not enqueue its virtual deadline");
  kernel.advance_absolute_time(alarm_deadline);

  cpu.registers()[0] = message;
  cpu.registers()[1] = 2;
  cpu.registers()[2] = 0;
  cpu.registers()[3] = 64;
  cpu.registers()[4] = kernel.process().bootstrap_port;
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(cpu, 0x80);
  constexpr const auto &alarm_reply_arguments =
      xnu792::mig::clock_reply::clock_alarm_reply_arguments;
  require(
      cpu.registers()[0] == 0 &&
          memory.read32(message + darwin::mig_wire::header_identifier_offset) ==
              std::optional<std::uint32_t>{xnu792::mig::clock_reply::id(
                  xnu792::mig::clock_reply::Routine::clock_alarm_reply)} &&
          memory.read32(message + alarm_reply_arguments[1].request_offset) ==
              std::optional<std::uint32_t>{darwin::mach::success} &&
          memory.read32(message + alarm_reply_arguments[2].request_offset) ==
              std::optional<std::uint32_t>{
                  darwin::mach::clock::time_relative} &&
          memory.read32(message + alarm_reply_arguments[3].request_offset) ==
              std::optional<std::uint32_t>{static_cast<std::uint32_t>(
                  alarm_deadline /
                  darwin::mach::clock::nanoseconds_per_second)} &&
          memory.read32(message + alarm_reply_arguments[3].request_offset +
                        sizeof(std::uint32_t)) ==
              std::optional<std::uint32_t>{static_cast<std::uint32_t>(
                  alarm_deadline %
                  darwin::mach::clock::nanoseconds_per_second)},
      "clock_alarm reply was not delivered through its Mach port");
}

void mach_kernel_timer_test() {
  AddressSpace memory;
  constexpr std::uint32_t message = 0x35000;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "Mach kernel timer message map failed");
  Dynarmic::ExclusiveMonitor monitor{2};
  Cpu receiver{0, memory, monitor};
  Cpu controller{1, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  controller.registers()[12] = static_cast<std::uint32_t>(-91);
  kernel.dispatch(controller, 0x80);
  const auto timer_port = controller.registers()[0];
  require(timer_port != 0, "mk_timer_create returned a null port");

  receiver.registers()[0] = message;
  receiver.registers()[1] = 2; // MACH_RCV_MSG
  receiver.registers()[2] = 0;
  receiver.registers()[3] = 64;
  receiver.registers()[4] = timer_port;
  receiver.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(receiver, 0x80);
  require(kernel.wait_reason(0).starts_with("mach_msg(port="),
          "timer receive did not block");

  constexpr std::uint64_t deadline = 50'000U;
  controller.registers()[0] = timer_port;
  controller.registers()[1] = static_cast<std::uint32_t>(deadline);
  controller.registers()[2] = static_cast<std::uint32_t>(deadline >> 32U);
  controller.registers()[12] = static_cast<std::uint32_t>(-93);
  kernel.dispatch(controller, 0x80);
  require(controller.registers()[0] == darwin::mach::success,
          "mk_timer_arm failed");
  require(kernel.next_timer_deadline() ==
              std::optional<std::uint64_t>{deadline},
          "armed Mach timer was absent from scheduler deadline selection");

  kernel.advance_absolute_time(deadline);
  require(kernel.deliver_pending_mach(receiver),
          "Mach timer expiration did not wake receiver");
  require(memory.read32(message + 4) == std::optional<std::uint32_t>{48},
          "Mach timer expiration message has the wrong size");
  require(memory.read32(message + 20) == std::optional<std::uint32_t>{0},
          "Mach timer expiration message has the wrong id");

  controller.registers()[0] = timer_port;
  controller.registers()[12] = static_cast<std::uint32_t>(-92);
  kernel.dispatch(controller, 0x80);
  require(controller.registers()[0] == darwin::mach::success,
          "mk_timer_destroy failed");
}

void mach_receive_timeout_test() {
  AddressSpace memory;
  constexpr std::uint32_t message = 0x36000;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "Mach receive timeout message map failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  kernel.dispatch(cpu, 0x80);
  const auto receive_port = cpu.registers()[0];
  require(receive_port != 0, "Mach receive timeout port allocation failed");

  const auto receive = [&](std::uint32_t timeout_milliseconds) {
    cpu.registers()[0] = message;
    cpu.registers()[1] = darwin::mach_message::option_receive |
                         darwin::mach_message::option_receive_timeout;
    cpu.registers()[2] = 0;
    cpu.registers()[3] = 64;
    cpu.registers()[4] = receive_port;
    cpu.registers()[5] = timeout_milliseconds;
    cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    kernel.dispatch(cpu, 0x80);
  };

  receive(0);
  require(cpu.registers()[0] == darwin::mach_message::receive_timed_out &&
              kernel.wait_reason(cpu.processor_id()) == "none",
          "zero-timeout Mach receive blocked instead of polling");

  receive(2);
  const auto deadline = kernel.next_timer_deadline();
  require(deadline && kernel.wait_reason(cpu.processor_id()).starts_with(
                          "mach_msg(port="),
          "finite Mach receive timeout did not enter a timed wait");
  kernel.advance_absolute_time(*deadline);
  require(kernel.deliver_pending_mach(cpu) &&
              cpu.registers()[0] == darwin::mach_message::receive_timed_out,
          "finite Mach receive timeout did not wake at its deadline");
}

void mach_kernel_timer_namespace_test() {
  AddressSpace parent_memory;
  AddressSpace child_memory;
  Dynarmic::ExclusiveMonitor parent_monitor{1};
  Dynarmic::ExclusiveMonitor child_monitor{1};
  Cpu parent_cpu{0, parent_memory, parent_monitor};
  Cpu child_cpu{0, child_memory, child_monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel parent{parent_memory, output};
  CompatibilityKernel child{child_memory, output};
  child.inherit_process_state(parent, 42);

  const auto create_timer = [](CompatibilityKernel &kernel, Cpu &cpu) {
    cpu.registers()[12] = static_cast<std::uint32_t>(-91);
    kernel.dispatch(cpu, 0x80);
    return cpu.registers()[0];
  };
  const auto parent_name = create_timer(parent, parent_cpu);
  const auto child_name = create_timer(child, child_cpu);
  require(parent_name != 0 && child_name != 0 && parent_name != child_name,
          "task-local timer allocation ignored independent IPC-space cursors");

  constexpr std::uint64_t parent_deadline = 100'000U;
  constexpr std::uint64_t child_deadline = 50'000U;
  const auto arm = [](CompatibilityKernel &kernel, Cpu &cpu, std::uint32_t name,
                      std::uint64_t deadline) {
    cpu.registers()[0] = name;
    cpu.registers()[1] = static_cast<std::uint32_t>(deadline);
    cpu.registers()[2] = static_cast<std::uint32_t>(deadline >> 32U);
    cpu.registers()[12] = static_cast<std::uint32_t>(-93);
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == darwin::mach::success,
            "task-local timer arm failed");
  };
  arm(parent, parent_cpu, parent_name, parent_deadline);
  arm(child, child_cpu, child_name, child_deadline);
  require(parent.next_timer_deadline() ==
              std::optional<std::uint64_t>{child_deadline},
          "task-local timer names addressed the wrong IPC objects");

  child_cpu.registers()[0] = child_name;
  child_cpu.registers()[12] = static_cast<std::uint32_t>(-92);
  child.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] == darwin::mach::success &&
              parent.next_timer_deadline() ==
                  std::optional<std::uint64_t>{parent_deadline},
          "destroying the child timer also destroyed the parent timer");

  parent_cpu.registers()[0] = parent_name;
  parent_cpu.registers()[12] = static_cast<std::uint32_t>(-92);
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == darwin::mach::success &&
              !parent.next_timer_deadline(),
          "parent task-local timer cleanup failed");
}

} // namespace

void run_timer_tests() {
  mach_timer_wait_test();
  mach_thread_switch_test();
  mach_clock_sleep_test();
  mach_clock_mig_test();
  mach_kernel_timer_test();
  mach_receive_timeout_test();
  mach_kernel_timer_namespace_test();
}

} // namespace ilemu::test::mach_suite
