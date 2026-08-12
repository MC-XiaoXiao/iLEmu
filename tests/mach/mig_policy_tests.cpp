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

void xnu_mig_adapter_test() {
  using namespace xnu792::mig;
  const auto insert_right = lookup_routine(3214);
  const auto get_bootstrap = lookup_routine(3409);
  const auto set_bootstrap = lookup_routine(3410);
  const auto thread_policy = lookup_routine(3617);
  const auto iterator_next = lookup_routine(
      static_cast<std::uint32_t>(iokit_abi::Message::IteratorNext));
  const auto matching_services = lookup_routine(2804);
  const auto service_open = lookup_routine(2815);
  const auto set_properties = lookup_routine(2828);
  const auto add_notification = lookup_routine(2849);
  const auto host_info = lookup_routine(200);
  const auto vm_allocate = lookup_routine(3801);
  const auto clock_get_time = lookup_routine(1000);
  const auto clock_alarm_reply = lookup_routine(3125107);
  const auto semaphore_signal = lookup_routine(617200);
  const auto vm_region_recurse = lookup_routine(3821);
  const auto vm_purgable_control = lookup_routine(3830);
  const auto thread_assign = lookup_routine(3621);
  const auto bootstrap_check_in = lookup_routine(402);
  const auto bootstrap_spawn = lookup_routine(413);
  const auto bootstrap_transfer = lookup_routine(411);
  const auto config_open = lookup_routine(20000);
  const auto config_get = lookup_routine(20010);
  const auto config_set_multiple = lookup_routine(20017);
  const auto config_notify_port = lookup_routine(20021);
  const auto config_snapshot = lookup_routine(20029);
  std::array<std::uint32_t, 6> notification_counts{};
  notification_counts[1] = 22;
  notification_counts[2] = 4;
  const auto notification_layout =
      add_notification
          ? compute_wire_layout(add_notification->arguments,
                                WireLayoutSide::Request, notification_counts)
          : std::nullopt;
  auto oversized_notification_counts = notification_counts;
  oversized_notification_counts[1] = 129;
  const auto oversized_notification_layout =
      add_notification ? compute_wire_layout(add_notification->arguments,
                                             WireLayoutSide::Request,
                                             oversized_notification_counts)
                       : std::nullopt;
  require(
      insert_right && insert_right->subsystem == Subsystem::MachPort &&
          insert_right->routine_name == "mach_port_insert_right" &&
          insert_right->arguments.size() == 3 &&
          insert_right->arguments[0].request_offset == 8 &&
          insert_right->arguments[1].request_offset == 48 &&
          insert_right->arguments[2].request_offset == 28 && get_bootstrap &&
          get_bootstrap->routine_name == "task_get_special_port" &&
          set_bootstrap &&
          set_bootstrap->routine_name == "task_set_special_port" &&
          thread_policy && thread_policy->routine_name == "thread_policy_set" &&
          iterator_next && iterator_next->subsystem == Subsystem::Iokit &&
          iterator_next->routine_name == "io_iterator_next" &&
          matching_services &&
          matching_services->arguments[1].count_prefix_size == 4 &&
          matching_services->arguments[1].element_size == 1 &&
          matching_services->arguments[1].request_count_offset == 36 &&
          matching_services->arguments[1].request_offset == 40 &&
          service_open && service_open->arguments[1].request_offset == 28 &&
          service_open->arguments[2].request_offset == 48 && set_properties &&
          set_properties->arguments[1].request_offset == 28 &&
          add_notification &&
          add_notification->arguments[1].count_prefix_size == 4 &&
          add_notification->arguments[1].element_size == 1 &&
          add_notification->arguments[1].request_count_offset == 52 &&
          add_notification->arguments[1].request_offset == 56 &&
          add_notification->arguments[2].count_prefix_size == 4 &&
          notification_layout && (*notification_layout)[1].count_offset == 52 &&
          (*notification_layout)[1].offset == 56 &&
          (*notification_layout)[2].count_offset == 84 &&
          (*notification_layout)[2].offset == 88 &&
          (*notification_layout)[4].count_offset == 92 &&
          (*notification_layout)[4].offset == 96 &&
          !oversized_notification_layout && host_info &&
          host_info->subsystem == Subsystem::MachHost &&
          host_info->routine_name == "host_info" &&
          host_info->arguments.size() == 3 &&
          host_info->arguments[2].name == "host_info_out" &&
          host_info->arguments[2].type == "host_info_t, CountInOut" &&
          host_info->arguments[2].attributes.empty() &&
          host_info->arguments[2].direction == ArgumentDirection::Out &&
          host_info->arguments[2].wire_type == WireType::VariableInline &&
          host_info->arguments[2].wire_size == 56 &&
          host_info->arguments[2].element_size == 4 &&
          host_info->arguments[1].request_offset == 32 &&
          host_info->arguments[2].request_count_offset == 36 &&
          host_info->arguments[2].reply_count_offset == 36 &&
          host_info->arguments[2].reply_offset == 40 && vm_allocate &&
          vm_allocate->subsystem == Subsystem::VmMap &&
          vm_allocate->routine_name == "vm_allocate" &&
          vm_allocate->arguments.size() == 4 &&
          vm_allocate->arguments[1].name == "address" &&
          vm_allocate->arguments[1].direction == ArgumentDirection::InOut &&
          vm_allocate->arguments[1].wire_type == WireType::Scalar &&
          vm_allocate->arguments[1].wire_size == 4 &&
          vm_allocate->arguments[1].request_offset == 32 &&
          vm_allocate->arguments[1].reply_offset == 36 && clock_get_time &&
          clock_get_time->subsystem == Subsystem::Clock &&
          clock_get_time->routine_name == "clock_get_time" &&
          clock_alarm_reply &&
          clock_alarm_reply->subsystem == Subsystem::ClockReply &&
          clock_alarm_reply->routine_name == "clock_alarm_reply" &&
          clock_alarm_reply->arguments.size() == 4 &&
          clock_alarm_reply->arguments[1].request_offset == 32 &&
          clock_alarm_reply->arguments[2].request_offset == 36 &&
          clock_alarm_reply->arguments[3].request_offset == 40 &&
          semaphore_signal &&
          semaphore_signal->subsystem == Subsystem::Semaphore &&
          semaphore_signal->routine_name == "semaphore_signal" &&
          vm_region_recurse &&
          vm_region_recurse->routine_name == "vm_region_recurse" &&
          vm_purgable_control &&
          vm_purgable_control->routine_name == "vm_purgable_control" &&
          thread_assign && thread_assign->routine_name == "thread_assign" &&
          bootstrap_check_in &&
          bootstrap_check_in->subsystem == Subsystem::Bootstrap &&
          bootstrap_check_in->routine_name == "check_in" &&
          bootstrap_check_in->arguments.size() == 4 &&
          bootstrap_check_in->arguments[2].name == "__token" &&
          bootstrap_check_in->arguments[2].attributes == "ServerAuditToken" &&
          bootstrap_check_in->arguments[1].wire_type == WireType::FixedInline &&
          bootstrap_check_in->arguments[1].wire_size == 128 &&
          bootstrap_check_in->arguments[1].request_offset == 32 &&
          bootstrap_check_in->arguments[3].type == "mach_port_move_receive_t" &&
          bootstrap_check_in->arguments[3].direction ==
              ArgumentDirection::Out &&
          bootstrap_check_in->arguments[3].wire_type == WireType::Port &&
          bootstrap_check_in->arguments[3].wire_size == 4 &&
          bootstrap_check_in->arguments[3].reply_offset == 28 &&
          bootstrap_spawn && bootstrap_spawn->routine_name == "spawn" &&
          bootstrap_spawn->arguments[2].name == "__chars" &&
          bootstrap_spawn->arguments[2].wire_type == WireType::OutOfLine &&
          bootstrap_spawn->arguments[2].request_offset == 28 &&
          bootstrap_spawn->arguments[2].request_count_offset == 48 &&
          bootstrap_spawn->arguments[3].request_offset == 52 &&
          bootstrap_spawn->arguments[4].request_offset == 56 &&
          bootstrap_spawn->arguments[5].request_offset == 60 &&
          bootstrap_spawn->arguments[6].request_offset == 68 &&
          bootstrap_transfer && bootstrap_transfer->arguments.size() == 6 &&
          bootstrap_transfer->arguments[1].reply_offset == 28 &&
          bootstrap_transfer->arguments[2].reply_offset == 40 &&
          bootstrap_transfer->arguments[3].reply_offset == 52 &&
          bootstrap_transfer->arguments[3].reply_count_offset == 96 &&
          bootstrap_transfer->arguments[4].reply_offset == 64 &&
          bootstrap_transfer->arguments[4].reply_count_offset == 100 &&
          bootstrap_transfer->arguments[5].reply_offset == 76 &&
          bootstrap_transfer->arguments[5].reply_count_offset == 104 &&
          config_open &&
          config_open->subsystem == Subsystem::SystemConfiguration &&
          config_open->routine_name == "configopen" &&
          config_open->arguments.size() == 5 &&
          config_open->arguments[1].wire_type == WireType::OutOfLine &&
          config_open->arguments[1].element_size == 1 &&
          config_open->arguments[1].request_offset == 28 &&
          config_open->arguments[1].request_count_offset == 60 &&
          config_open->arguments[2].wire_type == WireType::OutOfLine &&
          config_open->arguments[2].request_offset == 40 &&
          config_open->arguments[2].request_count_offset == 64 &&
          config_open->arguments[3].wire_type == WireType::Port &&
          config_open->arguments[3].reply_offset == 28 &&
          config_open->arguments[4].reply_offset == 48 && config_get &&
          config_get->routine_name == "configget" &&
          config_get->arguments[1].request_offset == 28 &&
          config_get->arguments[1].request_count_offset == 48 &&
          config_get->arguments[2].reply_offset == 28 &&
          config_get->arguments[2].reply_count_offset == 48 &&
          config_get->arguments[3].reply_offset == 52 &&
          config_get->arguments[4].reply_offset == 56 && config_set_multiple &&
          config_set_multiple->routine_name == "configset_m" &&
          config_set_multiple->arguments[1].request_offset == 28 &&
          config_set_multiple->arguments[1].request_count_offset == 72 &&
          config_set_multiple->arguments[2].request_offset == 40 &&
          config_set_multiple->arguments[2].request_count_offset == 76 &&
          config_set_multiple->arguments[3].request_offset == 52 &&
          config_set_multiple->arguments[3].request_count_offset == 80 &&
          config_notify_port &&
          config_notify_port->routine_name == "notifyviaport" &&
          config_notify_port->arguments[1].wire_type == WireType::Port &&
          config_notify_port->arguments[1].request_offset == 28 &&
          config_notify_port->arguments[2].request_offset == 48 &&
          config_snapshot && config_snapshot->routine_name == "snapshot" &&
          !lookup_routine(3820) && !lookup_routine(3620) &&
          !lookup_routine(0xffff'ffffU),
      "generated XNU 792 MIG routine identifiers are incorrect");
}

void libinfo_async_same_task_mach_test() {
  AddressSpace memory;
  constexpr std::uint32_t base = 0x58000;
  constexpr std::uint32_t rpc_message = base;
  constexpr std::uint32_t sender_message = base + 0x100;
  constexpr std::uint32_t receiver_message = base + 0x200;
  constexpr std::uint32_t mig_reply_port = 0x900;
  constexpr std::uint32_t event_pointer = 0x3801'a4c0U;
  constexpr std::uint32_t completion_size =
      darwin::mach_message::header_size + sizeof(std::uint32_t);
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "Libinfo same-task Mach memory map failed");

  Dynarmic::ExclusiveMonitor monitor{2};
  Cpu receiver{0, memory, monitor};
  Cpu sender{1, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  const auto call_mach_port = [&](Cpu &cpu, std::uint32_t send_size) {
    cpu.registers()[0] = rpc_message;
    cpu.registers()[1] = darwin::mach_message::option_send |
                         darwin::mach_message::option_receive;
    cpu.registers()[2] = send_size;
    cpu.registers()[3] = 64;
    cpu.registers()[4] = mig_reply_port;
    cpu.registers()[5] = 0;
    cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == darwin::mach::success &&
                memory.read32(rpc_message + 32) ==
                    std::optional<std::uint32_t>{darwin::mach::success},
            "Libinfo Mach-port RPC failed");
  };
  using MachPortRoutine = xnu792::mig::mach_port::Routine;
  require(memory.write32(rpc_message,
                         darwin::mach_message::bits(
                             darwin::mach_message::type_copy_send,
                             darwin::mach_message::type_make_send_once)) &&
              memory.write32(rpc_message + 4, 36) &&
              memory.write32(rpc_message + 8, kernel.process().task_port) &&
              memory.write32(rpc_message + 12, mig_reply_port) &&
              memory.write32(rpc_message + 20,
                             static_cast<std::uint32_t>(
                                 MachPortRoutine::mach_port_allocate)) &&
              memory.write32(rpc_message + 32, 1),
          "Libinfo receive-right allocation setup failed");
  call_mach_port(receiver, 36);
  const auto response_port = memory.read32(rpc_message + 36).value_or(0);
  require(response_port != 0,
          "Libinfo response receive right was not allocated");

  require(
      memory.write32(rpc_message,
                     darwin::mach_message::bits(
                         darwin::mach_message::type_copy_send,
                         darwin::mach_message::type_make_send_once, true)) &&
          memory.write32(rpc_message + 4, 52) &&
          memory.write32(rpc_message + 8, kernel.process().task_port) &&
          memory.write32(rpc_message + 12, mig_reply_port) &&
          memory.write32(rpc_message + 20,
                         static_cast<std::uint32_t>(
                             MachPortRoutine::mach_port_insert_right)) &&
          memory.write32(rpc_message + 24, 1) &&
          memory.write32(rpc_message + 28, response_port) &&
          memory.write32(rpc_message + 36,
                         darwin::mach_message::type_make_send << 16U) &&
          memory.write32(rpc_message + 40, 0) &&
          memory.write32(rpc_message + 44, 1) &&
          memory.write32(rpc_message + 48, response_port),
      "Libinfo response send-right insertion setup failed");
  call_mach_port(receiver, 52);

  receiver.registers()[0] = receiver_message;
  receiver.registers()[1] = darwin::mach_message::option_receive;
  receiver.registers()[2] = 0;
  receiver.registers()[3] = 64;
  receiver.registers()[4] = response_port;
  receiver.registers()[5] = 0;
  receiver.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(receiver, 0x80);
  require(
      kernel.wait_reason(receiver.processor_id()).starts_with("mach_msg(port="),
      "Libinfo response receiver did not block on its Mach port");

  require(memory.write32(sender_message,
                         darwin::mach_message::bits(
                             darwin::mach_message::type_copy_send)) &&
              memory.write32(sender_message + 4, completion_size) &&
              memory.write32(sender_message + 8, response_port) &&
              memory.write32(sender_message + 12, 0) &&
              memory.write32(sender_message + 16, 0) &&
              memory.write32(sender_message + 20, 0) &&
              memory.write32(sender_message + 24, event_pointer),
          "Libinfo 28-byte completion setup failed");
  sender.registers()[0] = sender_message;
  sender.registers()[1] = darwin::mach_message::option_send;
  sender.registers()[2] = completion_size;
  sender.registers()[3] = 0;
  sender.registers()[4] = 0;
  sender.registers()[5] = 0;
  sender.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(sender, 0x80);
  require(sender.registers()[0] == darwin::mach::success &&
              kernel.deliver_pending_mach(receiver) &&
              receiver.registers()[0] == darwin::mach::success &&
              memory.read32(receiver_message + 4) ==
                  std::optional<std::uint32_t>{completion_size} &&
              memory.read32(receiver_message + 20) ==
                  std::optional<std::uint32_t>{0} &&
              memory.read32(receiver_message + 24) ==
                  std::optional<std::uint32_t>{event_pointer},
          "Libinfo same-task completion message was not delivered intact");

  // mach_response_handle removes the event from its list, destroys this
  // receive right, and only then calls the completion callback.
  require(memory.write32(rpc_message,
                         darwin::mach_message::bits(
                             darwin::mach_message::type_copy_send,
                             darwin::mach_message::type_make_send_once)) &&
              memory.write32(rpc_message + 4, 36) &&
              memory.write32(rpc_message + 8, kernel.process().task_port) &&
              memory.write32(rpc_message + 12, mig_reply_port) &&
              memory.write32(rpc_message + 20,
                             static_cast<std::uint32_t>(
                                 MachPortRoutine::mach_port_destroy)) &&
              memory.write32(rpc_message + 32, response_port),
          "Libinfo response-port destroy setup failed");
  call_mach_port(receiver, 36);

  sender.registers()[0] = sender_message;
  sender.registers()[1] = darwin::mach_message::option_send;
  sender.registers()[2] = completion_size;
  sender.registers()[3] = 0;
  sender.registers()[4] = 0;
  sender.registers()[5] = 0;
  sender.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(sender, 0x80);
  require(sender.registers()[0] ==
              darwin::mach_message::send_invalid_destination,
          "destroyed Libinfo response port accepted a stale completion");
}

void mach_thread_policy_set_test() {
  using namespace darwin::mach::thread_policy;
  constexpr std::uint32_t message = 0x68000;
  constexpr std::uint32_t reply_port = 0x900;
  constexpr std::uint32_t request_size = static_cast<std::uint32_t>(
      request_policy_offset +
      time_constraint_policy_word_count * sizeof(std::uint32_t));

  AddressSpace memory;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "thread-policy message map failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};
  XnuScheduler scheduler;
  const XnuThreadId thread{kernel.process().pid, 0};
  require(scheduler.register_thread(thread),
          "thread-policy scheduler registration failed");
  bool preemption_queried = false;
  kernel.set_scheduler_preemption_query([&](std::size_t processor) {
    preemption_queried = processor == 0;
    return true;
  });
  kernel.set_thread_policy_handler([&](std::size_t processor,
                                       std::uint32_t flavor,
                                       std::span<const std::uint32_t> policy) {
    if (processor != 0 || flavor != time_constraint_policy ||
        policy.size() != time_constraint_policy_word_count) {
      return false;
    }
    const auto convert = [](std::uint32_t value) {
      return static_cast<std::uint64_t>(value) *
             xnu792::scheduler::default_guest_ticks_per_second /
             absolute_time_units_per_second;
    };
    return scheduler.set_realtime(thread,
                                  convert(policy[realtime_period_index]),
                                  convert(policy[realtime_computation_index]),
                                  convert(policy[realtime_constraint_index]),
                                  policy[realtime_preemptible_index] != 0);
  });

  const std::array<std::uint32_t, request_size / sizeof(std::uint32_t)> request{
      0x1513,
      request_size,
      kernel.process().thread_port,
      reply_port,
      0,
      policy_set_message,
      0,
      1,
      time_constraint_policy,
      static_cast<std::uint32_t>(time_constraint_policy_word_count),
      0,
      50'000,
      100'000,
      1,
  };
  for (std::size_t index = 0; index < request.size(); ++index) {
    require(memory.write32(message + static_cast<std::uint32_t>(
                                         index * sizeof(std::uint32_t)),
                           request[index]),
            "thread-policy request write failed");
  }
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  cpu.registers()[0] = message;
  cpu.registers()[1] = 3;
  cpu.registers()[2] = request_size;
  cpu.registers()[3] = request_size;
  cpu.registers()[4] = reply_port;
  kernel.dispatch(cpu, 0x80);

  const auto scheduling_info = scheduler.info(thread);
  require(cpu.registers()[0] == 0 &&
              memory.read32(message + 4) ==
                  std::optional<std::uint32_t>{simple_reply_size} &&
              memory.read32(message + 32) == std::optional<std::uint32_t>{0} &&
              scheduling_info && scheduling_info->realtime &&
              scheduling_info->realtime_computation ==
                  xnu792::scheduler::minimum_realtime_computation_ticks &&
              preemption_queried,
          "firmware MIG thread_policy_set did not reach the XNU scheduler");
}

} // namespace

void run_mig_policy_tests() {
  xnu_mig_adapter_test();
  libinfo_async_same_task_mach_test();
  mach_thread_policy_set_test();
}

} // namespace ilemu::test::mach_suite
