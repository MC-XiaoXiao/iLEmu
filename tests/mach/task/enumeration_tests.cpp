#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/output.hpp"
#include "ilemu/task_mig_ids.hpp"

#include "test_support.hpp"

#include "../suite.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>

namespace ilemu::test::mach_suite {
namespace {

using ::ilemu::test::require;

void dispatch_mig(Cpu &cpu, CompatibilityKernel &kernel, std::uint32_t message,
                  std::uint32_t send_size, std::uint32_t receive_size) {
  cpu.registers()[0] = message;
  cpu.registers()[1] = 3;
  cpu.registers()[2] = send_size;
  cpu.registers()[3] = receive_size;
  cpu.registers()[4] = 0x900U;
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(cpu, 0x80);
}

void task_threads_enumeration_test() {
  constexpr std::uint32_t message = 0x4d000U;
  constexpr std::uint32_t reply_port = 0x900U;
  constexpr std::uint32_t thread_state_count = 17;
  constexpr std::uint32_t create_request_size =
      xnu792::mig::task::thread_create_running_arguments[2].request_offset +
      thread_state_count * sizeof(std::uint32_t);
  constexpr std::uint32_t threads_request_size =
      darwin::mig_wire::message_header_size;

  AddressSpace memory;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "task_threads message map failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};
  kernel.set_thread_create_handler([](const auto &, std::uint32_t) {
    return std::optional<std::size_t>{1};
  });

  require(
      memory.write32(message + darwin::mig_wire::header_bits_offset,
                     darwin::mig_wire::message_bits(
                         darwin::mig_wire::disposition_copy_send,
                         darwin::mig_wire::disposition_make_send_once)) &&
          memory.write32(message + darwin::mig_wire::header_size_offset,
                         create_request_size) &&
          memory.write32(message + darwin::mig_wire::header_remote_port_offset,
                         kernel.process().task_port) &&
          memory.write32(message + darwin::mig_wire::header_local_port_offset,
                         reply_port) &&
          memory.write32(
              message + darwin::mig_wire::header_identifier_offset,
              static_cast<std::uint32_t>(
                  xnu792::mig::task::Routine::thread_create_running)) &&
          memory.write32(
              message + xnu792::mig::task::thread_create_running_arguments[1]
                            .request_offset,
              1) &&
          memory.write32(
              message + xnu792::mig::task::thread_create_running_arguments[2]
                            .request_count_offset,
              thread_state_count),
      "thread_create_running request setup failed");
  dispatch_mig(cpu, kernel, message, create_request_size,
               AddressSpace::page_size);
  require(cpu.registers()[0] == 0, "second guest thread creation failed");

  require(
      memory.write32(message + darwin::mig_wire::header_bits_offset,
                     darwin::mig_wire::message_bits(
                         darwin::mig_wire::disposition_copy_send,
                         darwin::mig_wire::disposition_make_send_once)) &&
          memory.write32(message + darwin::mig_wire::header_size_offset,
                         threads_request_size) &&
          memory.write32(message + darwin::mig_wire::header_remote_port_offset,
                         kernel.process().task_port) &&
          memory.write32(message + darwin::mig_wire::header_local_port_offset,
                         reply_port) &&
          memory.write32(message + darwin::mig_wire::header_identifier_offset,
                         static_cast<std::uint32_t>(
                             xnu792::mig::task::Routine::task_threads)),
      "task_threads request setup failed");
  dispatch_mig(cpu, kernel, message, threads_request_size,
               AddressSpace::page_size);

  const auto array_address =
      memory
          .read32(message +
                  xnu792::mig::task::task_threads_arguments[1].reply_offset)
          .value_or(0);
  const auto count =
      memory
          .read32(
              message +
              xnu792::mig::task::task_threads_arguments[1].reply_count_offset)
          .value_or(0);
  require(
      cpu.registers()[0] == 0 &&
          memory.read32(message + darwin::mig_wire::header_size_offset) ==
              std::optional<std::uint32_t>{52} &&
          count == 2 && array_address != 0 &&
          memory.read32(array_address).value_or(0) != 0 &&
          memory.read32(array_address + sizeof(std::uint32_t)).value_or(0) !=
              0 &&
          memory.read32(array_address) !=
              memory.read32(array_address + sizeof(std::uint32_t)),
      "task_threads did not return both task-local thread send rights");
}

} // namespace

void run_task_tests() { task_threads_enumeration_test(); }

} // namespace ilemu::test::mach_suite
