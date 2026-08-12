#include "suite.hpp"

#include "test_support.hpp"

#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/output.hpp"

#include <cstdint>
#include <optional>
#include <sstream>

namespace ilemu::test::mach_suite {
namespace {

using ::ilemu::test::require;

void arm32_mach_vm_allocate_test() {
  constexpr std::uint32_t message = 0x51000U;
  constexpr std::uint32_t request_size = 44U;
  constexpr std::uint32_t reply_size = 40U;
  constexpr std::uint32_t allocation_size = 0xc000U;
  AddressSpace memory;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "mach_vm_allocate message map failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  require(memory.write32(message,
                         darwin::mig_wire::message_bits(
                             darwin::mig_wire::disposition_copy_send,
                             darwin::mig_wire::disposition_make_send_once)) &&
              memory.write32(message + 4, request_size) &&
              memory.write32(message + 8, kernel.process().task_port) &&
              memory.write32(message + 12, 0x930U) &&
              memory.write32(message + 20, 4800U) &&
              memory.write32(message + 24, 0) &&
              memory.write32(message + 28, 1) &&
              memory.write32(message + 32, 0x11eU) &&
              memory.write32(message + 36, allocation_size) &&
              memory.write32(message + 40, 0x33000003U),
          "ARM32 mach_vm_allocate request setup failed");
  cpu.registers()[0] = message;
  cpu.registers()[1] = darwin::mach_message::option_send |
                       darwin::mach_message::option_receive;
  cpu.registers()[2] = request_size;
  cpu.registers()[3] = 48;
  cpu.registers()[4] = 0x930U;
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(cpu, 0x80);

  const auto address = memory.read32(message + 36).value_or(0);
  require(cpu.registers()[0] == darwin::mach::success &&
              memory.read32(message + 4) ==
                  std::optional<std::uint32_t>{reply_size} &&
              memory.read32(message + 32) ==
                  std::optional<std::uint32_t>{darwin::mach::success} &&
              address != 0 && memory.mapped(address, allocation_size),
          "ARM32 mach_vm_allocate did not return mapped memory");
}

} // namespace

void run_vm_allocate_tests() { arm32_mach_vm_allocate_test(); }

} // namespace ilemu::test::mach_suite
