#include "suite.hpp"

#include "test_support.hpp"

#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/mach_port_mig_ids.hpp"
#include "ilemu/mach_port_object.hpp"
#include "ilemu/output.hpp"

#include <cstdint>
#include <optional>
#include <sstream>

namespace ilemu::test::mach_suite {
namespace {

using ::ilemu::test::require;

void set_port_queue_limit_test() {
  AddressSpace memory;
  constexpr std::uint32_t buffer = 0x49000U;
  require(memory.map(buffer, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "port-limit buffer map failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  kernel.dispatch(cpu, 0x80);
  const auto receive_name = cpu.registers()[0];
  require(receive_name != 0, "port-limit receive right allocation failed");

  const auto set_attributes_id = xnu792::mig::mach_port::id(
      xnu792::mig::mach_port::Routine::mach_port_set_attributes);
  const auto set_limit = [&](std::uint32_t limit) {
    // Match the firmware's kernelrpc form: msgh_size and reply port are zero;
    // the trap arguments carry the authoritative send size.
    require(
        memory.write32(buffer, 0x13U) && memory.write32(buffer + 4, 0) &&
            memory.write32(buffer + 8, kernel.process().task_port) &&
            memory.write32(buffer + 12, 0) &&
            memory.write32(buffer + 20, set_attributes_id) &&
            memory.write32(buffer + 24, 0) && memory.write32(buffer + 28, 1) &&
            memory.write32(buffer + 32, receive_name) &&
            memory.write32(buffer + 36, 1) && memory.write32(buffer + 40, 1) &&
            memory.write32(buffer + 44, limit),
        "port-limit set_attributes request setup failed");
    cpu.registers()[0] = buffer;
    cpu.registers()[1] = 1; // MACH_SEND_MSG
    cpu.registers()[2] = 48;
    cpu.registers()[3] = 0;
    cpu.registers()[4] = 0;
    cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == 0,
            "port-limit set_attributes Mach trap failed");
    return memory.read32(buffer + 32).value_or(0xffff'ffffU);
  };

  require(set_limit(xnu792::ipc::small_queue_limit) == darwin::mach::success &&
              set_limit(xnu792::ipc::framework_queue_limit) ==
                  darwin::mach::success &&
              set_limit(xnu792::ipc::maximum_queue_limit) ==
                  darwin::mach::success,
          "target iPhone port queue limit was rejected");

  const auto get_attributes_id = xnu792::mig::mach_port::id(
      xnu792::mig::mach_port::Routine::mach_port_get_attributes);
  require(memory.write32(buffer, 0x1513U) && memory.write32(buffer + 4, 44) &&
              memory.write32(buffer + 8, kernel.process().task_port) &&
              memory.write32(buffer + 12, 0x900U) &&
              memory.write32(buffer + 20, get_attributes_id) &&
              memory.write32(buffer + 24, 0) &&
              memory.write32(buffer + 28, 1) &&
              memory.write32(buffer + 32, receive_name) &&
              memory.write32(buffer + 36, 1) && memory.write32(buffer + 40, 1),
          "port-limit get_attributes request setup failed");
  cpu.registers()[0] = buffer;
  cpu.registers()[1] = 3; // MACH_SEND_MSG | MACH_RCV_MSG
  cpu.registers()[2] = 44;
  cpu.registers()[3] = 44;
  cpu.registers()[4] = 0x900U;
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(cpu, 0x80);
  require(
      cpu.registers()[0] == 0 &&
          memory.read32(buffer + 32) ==
              std::optional<std::uint32_t>{darwin::mach::success} &&
          memory.read32(buffer + 36) == std::optional<std::uint32_t>{1} &&
          memory.read32(buffer + 40) ==
              std::optional<std::uint32_t>{xnu792::ipc::maximum_queue_limit},
      "port-limit get_attributes did not return the stored limit");
  require(set_limit(xnu792::ipc::maximum_queue_limit + 1U) ==
                  darwin::mach::invalid_value &&
              stream.str().find("[mach] port-limit") != std::string::npos,
          "out-of-range port queue limit or trace result mismatch");

  require(set_limit(1) == darwin::mach::success &&
              memory.write32(buffer,
                             darwin::mach_message::bits(
                                 darwin::mach_message::type_make_send)) &&
              memory.write32(buffer + 4, darwin::mach_message::header_size) &&
              memory.write32(buffer + 8, receive_name) &&
              memory.write32(buffer + 12, 0) &&
              memory.write32(buffer + 16, 0) &&
              memory.write32(buffer + 20, 0x1234U),
          "bounded port queue message setup failed");
  const auto send = [&](std::uint32_t options) {
    cpu.registers()[0] = buffer;
    cpu.registers()[1] = options;
    cpu.registers()[2] = darwin::mach_message::header_size;
    cpu.registers()[3] = 0;
    cpu.registers()[4] = 0;
    cpu.registers()[5] = 0;
    cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    kernel.dispatch(cpu, 0x80);
    return cpu.registers()[0];
  };
  require(send(darwin::mach_message::option_send) == darwin::mach::success &&
              send(darwin::mach_message::option_send |
                   darwin::mach_message::option_send_timeout) ==
                  darwin::mach_message::send_timed_out,
          "zero-timeout send ignored the receive port queue limit");
}

} // namespace

void run_port_limit_tests() { set_port_queue_limit_test(); }

} // namespace ilemu::test::mach_suite
