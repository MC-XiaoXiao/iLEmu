#include <cstdint>
#include <sstream>

#include <dynarmic/interface/exclusive_monitor.h>

#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/memory_permission.hpp"
#include "test_support.hpp"

namespace {

using ilemu::AddressSpace;
using ilemu::CompatibilityKernel;
using ilemu::Cpu;
using ilemu::MemoryPermission;
using ilemu::Output;
using ilemu::test::require;

constexpr std::uint32_t carry_flag = 1U << 29U;
constexpr std::uint32_t ranges_address = 0x10000U;
constexpr std::uint32_t shared_region = 0x30000000U;

void shared_region_make_private_test() {
  AddressSpace memory;
  require(memory.map(ranges_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write) &&
              memory.map(shared_region, 3U * AddressSpace::page_size,
                         MemoryPermission::Read),
          "shared-region test mappings failed");
  require(memory.write64(ranges_address,
                          shared_region + AddressSpace::page_size) &&
              memory.write64(ranges_address + sizeof(std::uint64_t),
                             AddressSpace::page_size),
          "shared-region range write failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  cpu.registers()[0] = 1;
  cpu.registers()[1] = ranges_address;
  cpu.registers()[12] = 300;
  kernel.dispatch(cpu, 0x80);
  require((cpu.cpsr() & carry_flag) == 0 && cpu.registers()[0] == 0 &&
              !memory.mapped(shared_region, AddressSpace::page_size) &&
              memory.mapped(shared_region + AddressSpace::page_size,
                            AddressSpace::page_size) &&
              !memory.mapped(shared_region + 2U * AddressSpace::page_size,
                             AddressSpace::page_size),
          "shared-region privatization did not release only non-retained pages");

  require(memory.map(shared_region + 2U * AddressSpace::page_size,
                      AddressSpace::page_size, MemoryPermission::Read) &&
              memory.write64(ranges_address, 0x3ffff000U) &&
              memory.write64(ranges_address + sizeof(std::uint64_t),
                             2U * AddressSpace::page_size),
          "shared-region invalid-range setup failed");
  cpu.registers()[0] = 1;
  cpu.registers()[1] = ranges_address;
  kernel.dispatch(cpu, 0x80);
  require((cpu.cpsr() & carry_flag) != 0 && cpu.registers()[0] == 22 &&
              memory.mapped(shared_region + AddressSpace::page_size,
                            AddressSpace::page_size) &&
              memory.mapped(shared_region + 2U * AddressSpace::page_size,
                            AddressSpace::page_size),
          "invalid shared-region range changed existing mappings");

  cpu.registers()[0] = 1;
  cpu.registers()[1] = 0xdead0000U;
  kernel.dispatch(cpu, 0x80);
  require((cpu.cpsr() & carry_flag) != 0 && cpu.registers()[0] == 14,
          "unreadable shared-region ranges did not return EFAULT");

  cpu.registers()[0] = 0;
  cpu.registers()[1] = 0;
  kernel.dispatch(cpu, 0x80);
  require((cpu.cpsr() & carry_flag) == 0 &&
              !memory.mapped(shared_region + AddressSpace::page_size,
                             AddressSpace::page_size) &&
              !memory.mapped(shared_region + 2U * AddressSpace::page_size,
                             AddressSpace::page_size),
          "zero-range shared-region privatization did not release all pages");
}

} // namespace

int main() {
  return ilemu::test::run_suite("shared_region", shared_region_make_private_test);
}
