#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/mach_arm_thread_abi.hpp"
#include "ilemu/mach_port_mig_ids.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/output.hpp"
#include "ilemu/thread_act_mig_ids.hpp"

#include "test_support.hpp"

#include "../suite.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>

namespace ilemu::test::mach_suite {
namespace {

using ::ilemu::test::require;

void arm_thread_get_state_test() {
  constexpr std::uint32_t message = 0x4e000U;
  constexpr std::uint32_t request_size = 40;
  constexpr std::uint32_t reply_size =
      40 + darwin::arm_thread::general_state_word_count * sizeof(std::uint32_t);
  constexpr std::uint32_t reply_port = 0x910U;

  AddressSpace memory;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "thread_get_state message map failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  darwin::arm_thread::GeneralState expected{};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    expected[index] = 0x1000U + static_cast<std::uint32_t>(index);
  }
  kernel.set_thread_state_query(
      [expected](std::uint32_t pid, std::uint32_t slot, std::uint32_t flavor)
          -> std::optional<darwin::arm_thread::GeneralState> {
        return pid == 1 && slot == 0 &&
                       flavor == darwin::arm_thread::general_state_flavor
                   ? std::optional{expected}
                   : std::nullopt;
      });

  const auto &arguments = xnu792::mig::thread_act::thread_get_state_arguments;
  require(
      memory.write32(message + darwin::mig_wire::header_bits_offset,
                     darwin::mig_wire::message_bits(
                         darwin::mig_wire::disposition_copy_send,
                         darwin::mig_wire::disposition_make_send_once)) &&
          memory.write32(message + darwin::mig_wire::header_size_offset,
                         request_size) &&
          memory.write32(message + darwin::mig_wire::header_remote_port_offset,
                         kernel.process().thread_port) &&
          memory.write32(message + darwin::mig_wire::header_local_port_offset,
                         reply_port) &&
          memory.write32(
              message + darwin::mig_wire::header_identifier_offset,
              static_cast<std::uint32_t>(
                  xnu792::mig::thread_act::Routine::thread_get_state)) &&
          memory.write32(message + arguments[1].request_offset,
                         darwin::arm_thread::general_state_flavor) &&
          memory.write32(message + arguments[2].request_count_offset,
                         darwin::arm_thread::general_state_word_count),
      "thread_get_state request setup failed");

  cpu.registers()[0] = message;
  cpu.registers()[1] = 3;
  cpu.registers()[2] = request_size;
  cpu.registers()[3] = reply_size;
  cpu.registers()[4] = reply_port;
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(cpu, 0x80);

  require(cpu.registers()[0] == 0 &&
              memory.read32(message + darwin::mig_wire::header_size_offset) ==
                  std::optional<std::uint32_t>{reply_size} &&
              memory.read32(message + arguments[2].reply_count_offset) ==
                  std::optional<std::uint32_t>{
                      darwin::arm_thread::general_state_word_count} &&
              memory.read32(message + arguments[2].reply_offset) ==
                  std::optional<std::uint32_t>{expected[0]} &&
              memory.read32(message + arguments[2].reply_offset +
                            darwin::arm_thread::cpsr_index *
                                sizeof(std::uint32_t)) ==
                  std::optional<std::uint32_t>{
                      expected[darwin::arm_thread::cpsr_index]},
          "thread_get_state did not return the queried ARM register state");
}

void thread_self_terminate_test() {
  constexpr std::uint32_t message = 0x4f000U;
  AddressSpace memory;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "thread_terminate message map failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  bool retired = false;
  kernel.set_thread_terminate_handler(
      [&](std::uint32_t pid, std::size_t processor) {
        retired = pid == kernel.process().pid && processor == 0;
        return retired;
      });

  require(
      memory.write32(message + darwin::mig_wire::header_bits_offset,
                     darwin::mig_wire::message_bits(
                         darwin::mig_wire::disposition_copy_send,
                         darwin::mig_wire::disposition_make_send_once)) &&
          memory.write32(message + darwin::mig_wire::header_size_offset, 36) &&
          memory.write32(message + darwin::mig_wire::header_remote_port_offset,
                         kernel.process().task_port) &&
          memory.write32(message + darwin::mig_wire::header_local_port_offset,
                         0x920U) &&
          memory.write32(
              message + darwin::mig_wire::header_identifier_offset,
              xnu792::mig::mach_port::id(
                  xnu792::mig::mach_port::Routine::mach_port_deallocate)) &&
          memory.write32(
              message +
                  xnu792::mig::mach_port::mach_port_deallocate_arguments[1]
                      .request_offset,
              kernel.process().thread_port),
      "thread-self send-right deallocation setup failed");
  cpu.registers()[0] = message;
  cpu.registers()[1] = darwin::mach_message::option_send |
                       darwin::mach_message::option_receive;
  cpu.registers()[2] = 36;
  cpu.registers()[3] = 40;
  cpu.registers()[4] = 0x920U;
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(cpu, 0x80);
  require(memory.read32(message + 32) ==
              std::optional<std::uint32_t>{darwin::mach::success},
          "thread-self send right could not be released");

  cpu.registers()[12] = static_cast<std::uint32_t>(-27);
  kernel.dispatch(cpu, 0x80);
  const auto renewed_thread_self = cpu.registers()[0];
  require(renewed_thread_self != xnu792::ipc::null_name,
          "thread_self_trap did not copy out a fresh send right");
  require(
      memory.write32(message + darwin::mig_wire::header_bits_offset,
                     darwin::mig_wire::message_bits(
                         darwin::mig_wire::disposition_move_send,
                         darwin::mig_wire::disposition_make_send_once)) &&
          memory.write32(message + darwin::mig_wire::header_size_offset, 24) &&
          memory.write32(message + darwin::mig_wire::header_remote_port_offset,
                         renewed_thread_self) &&
          memory.write32(message + darwin::mig_wire::header_local_port_offset,
                         0x920U) &&
          memory.write32(
              message + darwin::mig_wire::header_identifier_offset,
              static_cast<std::uint32_t>(
                  xnu792::mig::thread_act::Routine::thread_terminate)),
      "thread_terminate request setup failed");

  cpu.registers()[0] = message;
  cpu.registers()[1] = darwin::mach_message::option_send |
                       darwin::mach_message::option_receive;
  cpu.registers()[2] = 24;
  cpu.registers()[3] = 36;
  cpu.registers()[4] = 0x920U;
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(cpu, 0x80);

  require(retired &&
              Dynarmic::Has(cpu.consume_requested_halt_reason(),
                            Dynarmic::HaltReason::UserDefined1),
          "self thread_terminate did not retire the scheduled thread");
}

} // namespace

void run_thread_tests() {
  arm_thread_get_state_test();
  thread_self_terminate_test();
}

} // namespace ilemu::test::mach_suite
