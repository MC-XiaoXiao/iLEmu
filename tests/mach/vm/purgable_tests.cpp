#include "suite.hpp"

#include "test_support.hpp"

#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/output.hpp"
#include "ilemu/vm_map_mig_ids.hpp"

#include <cstdint>
#include <optional>
#include <sstream>

namespace ilemu::test::mach_suite {
namespace {

using ::ilemu::test::require;

void vm_purgable_state_round_trip_test() {
  constexpr std::uint32_t message = 0x51000U;
  constexpr std::uint32_t allocation = 0x62000U;
  constexpr std::uint32_t request_size = 44U;
  constexpr std::uint32_t reply_size = 40U;
  constexpr std::uint32_t reply_port = 0x930U;
  constexpr std::uint32_t identifier = 3830U;
  AddressSpace memory;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write) &&
              memory.map(allocation, AddressSpace::page_size,
                         MemoryPermission::Read | MemoryPermission::Write) &&
              memory.write32(allocation, 0xfeedfaceU),
          "vm_purgable_control memory setup failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  const auto call = [&](std::uint32_t control, std::uint32_t state) {
    require(memory.write32(
                message,
                darwin::mig_wire::message_bits(
                    darwin::mig_wire::disposition_copy_send,
                    darwin::mig_wire::disposition_make_send_once)) &&
                memory.write32(message + 4, request_size) &&
                memory.write32(message + 8, kernel.process().task_port) &&
                memory.write32(message + 12, reply_port) &&
                memory.write32(message + 20, identifier) &&
                memory.write32(message + 24, 0) &&
                memory.write32(message + 28, 1) &&
                memory.write32(message + 32, allocation) &&
                memory.write32(message + 36, control) &&
                memory.write32(message + 40, state),
            "vm_purgable_control request setup failed");
    cpu.registers()[0] = message;
    cpu.registers()[1] = darwin::mach_message::option_send |
                         darwin::mach_message::option_receive;
    cpu.registers()[2] = request_size;
    cpu.registers()[3] = 48U;
    cpu.registers()[4] = reply_port;
    cpu.registers()[12] = static_cast<std::uint32_t>(-31);
    kernel.dispatch(cpu, 0x80);
    require(cpu.registers()[0] == darwin::mach::success &&
                memory.read32(message + 4) ==
                    std::optional<std::uint32_t>{reply_size} &&
                memory.read32(message + 32) ==
                    std::optional<std::uint32_t>{darwin::mach::success},
            "vm_purgable_control call failed");
    return memory.read32(message + 36).value_or(0xffffffffU);
  };

  require(call(0U, 1U) == 0U && call(1U, 0U) == 1U &&
              memory.read32(allocation) ==
                  std::optional<std::uint32_t>{0xfeedfaceU},
          "vm_purgable_control did not preserve state and memory");
}

} // namespace

void run_vm_purgable_tests() { vm_purgable_state_round_trip_test(); }

} // namespace ilemu::test::mach_suite
