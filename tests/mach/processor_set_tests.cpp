#include "suite.hpp"

#include "test_support.hpp"

#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/host_priv_mig_ids.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/output.hpp"

#include <cstdint>
#include <optional>
#include <sstream>

namespace ilemu::test::mach_suite {
namespace {

using ::ilemu::test::require;

void host_processor_sets_test() {
  AddressSpace memory;
  constexpr std::uint32_t buffer = 0x4b000U;
  require(memory.map(buffer, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "processor-set message buffer map failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  require(memory.write32(buffer, 0x1513U) && memory.write32(buffer + 4, 24) &&
              memory.write32(buffer + 8, kernel.process().host_port) &&
              memory.write32(buffer + 12, 0x900U) &&
              memory.write32(
                  buffer + 20,
                  xnu792::mig::host_priv::id(
                      xnu792::mig::host_priv::Routine::host_processor_sets)),
          "host_processor_sets request setup failed");
  cpu.registers()[0] = buffer;
  cpu.registers()[1] = 3;
  cpu.registers()[2] = 24;
  cpu.registers()[3] = 128;
  cpu.registers()[4] = 0x900U;
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(cpu, 0x80);

  const auto array_address = memory.read32(buffer + 28).value_or(0);
  require(cpu.registers()[0] == 0 &&
              memory.read32(buffer + 4) == std::optional<std::uint32_t>{52} &&
              memory.read32(buffer + 32) == std::optional<std::uint32_t>{1} &&
              memory.read32(buffer + 36) ==
                  std::optional<std::uint32_t>{
                      darwin::mig_wire::ool_ports_descriptor_metadata(
                          darwin::mig_wire::disposition_move_send, true)} &&
              memory.read32(buffer + 48) == std::optional<std::uint32_t>{1} &&
              array_address != 0 &&
              memory.read32(array_address).value_or(0) != 0,
          "host_processor_sets did not return the default set right");
}

} // namespace

void run_processor_set_tests() { host_processor_sets_test(); }

} // namespace ilemu::test::mach_suite
