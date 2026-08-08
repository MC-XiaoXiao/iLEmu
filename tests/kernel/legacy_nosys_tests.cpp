#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#include "ilemu/cpu.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/output.hpp"

#include "../support/test_support.hpp"

namespace {

using namespace ilemu;
using ilemu::test::require;

constexpr std::uint32_t carry_flag = 1U << 29U;
constexpr std::uint32_t nosys = 322U;
constexpr std::uint32_t enosys = 78U;
constexpr std::uint32_t efault = 14U;
constexpr std::uint32_t einval = 22U;

void dispatch_bsd(Cpu &cpu, CompatibilityKernel &kernel,
                  std::uint32_t command, std::uint32_t address) {
  cpu.registers()[0] = command;
  cpu.registers()[1] = address;
  cpu.registers()[12] = nosys;
  cpu.set_cpsr(cpu.cpsr() & ~carry_flag);
  kernel.dispatch(cpu, 0x80U);
}

std::filesystem::path make_system_version_rootfs(std::string_view build) {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto rootfs = std::filesystem::temp_directory_path() /
                      ("ilemu-iopolicy-" + std::string{build} + "-" +
                       std::to_string(nonce));
  std::filesystem::create_directories(
      rootfs / "System/Library/CoreServices");
  std::ofstream plist{rootfs /
                      "System/Library/CoreServices/SystemVersion.plist"};
  plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<plist version=\"1.0\"><dict>"
           "<key>ProductBuildVersion</key><string>"
        << build
        << "</string></dict></plist>\n";
  return rootfs;
}

void legacy_nosys_returns_error_without_halt_test() {
  const auto rootfs = make_system_version_rootfs("3A109a");
  AddressSpace memory;
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output, rootfs};

  cpu.registers()[0] = 0xfeedbeefU;
  cpu.registers()[12] = nosys;
  cpu.set_cpsr(cpu.cpsr() & ~carry_flag);
  kernel.dispatch(cpu, 0x80U);

  require(cpu.registers()[0] == enosys,
          "XNU nosys slot did not return ENOSYS");
  require((cpu.cpsr() & carry_flag) != 0,
          "XNU nosys slot did not set the BSD error carry flag");
  require(!Dynarmic::Has(cpu.consume_requested_halt_reason(),
                         Dynarmic::HaltReason::UserDefined4),
          "XNU nosys slot incorrectly halted the guest CPU");
  std::filesystem::remove_all(rootfs);
}

void legacy_iopolicysys_is_version_gated_test() {
  const auto rootfs = make_system_version_rootfs("5A347");

  AddressSpace memory;
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output, rootfs};
  constexpr std::uint32_t parameter_address = 0x1000;
  require(memory.map(parameter_address, 0x1000,
                      MemoryPermission::Read | MemoryPermission::Write),
          "failed to map iopolicysys parameter buffer");

  memory.write32(parameter_address, 0); // process scope
  memory.write32(parameter_address + 4, 0); // disk iotype
  memory.write32(parameter_address + 8, 3); // throttle
  dispatch_bsd(cpu, kernel, 2, parameter_address); // IOPOL_CMD_SET
  require(cpu.registers()[0] == 0 && (cpu.cpsr() & carry_flag) == 0,
          "5A347 iopolicysys SET did not succeed");

  memory.write32(parameter_address + 8, 0);
  dispatch_bsd(cpu, kernel, 1, parameter_address); // IOPOL_CMD_GET
  require(cpu.registers()[0] == 0 &&
              memory.read32(parameter_address + 8).value_or(0) == 3,
          "5A347 iopolicysys GET did not return the process policy");

  memory.write32(parameter_address, 1); // current-thread scope
  memory.write32(parameter_address + 8, 2); // passive
  dispatch_bsd(cpu, kernel, 2, parameter_address);
  require(cpu.registers()[0] == 0 && (cpu.cpsr() & carry_flag) == 0,
          "5A347 iopolicysys thread SET did not succeed");
  memory.write32(parameter_address + 8, 0);
  dispatch_bsd(cpu, kernel, 1, parameter_address);
  require(cpu.registers()[0] == 0 &&
              memory.read32(parameter_address + 8).value_or(0) == 2,
          "5A347 iopolicysys GET did not return the thread policy");

  memory.write32(parameter_address, 0); // restore process scope
  memory.write32(parameter_address + 4, 1); // later iotype, not 5A347 ABI
  dispatch_bsd(cpu, kernel, 1, parameter_address);
  require(cpu.registers()[0] == einval && (cpu.cpsr() & carry_flag) != 0,
          "5A347 iopolicysys exposed a later iotype");

  memory.write32(parameter_address + 4, 0);
  memory.write32(parameter_address + 8, 4); // later policy value
  dispatch_bsd(cpu, kernel, 2, parameter_address);
  require(cpu.registers()[0] == einval && (cpu.cpsr() & carry_flag) != 0,
          "5A347 iopolicysys accepted a later policy value");

  dispatch_bsd(cpu, kernel, 1, 0xfffffff0U);
  require(cpu.registers()[0] == efault && (cpu.cpsr() & carry_flag) != 0,
          "5A347 iopolicysys did not reject an invalid pointer");

  std::filesystem::remove_all(rootfs);
}

} // namespace

void legacy_syscall_abi_suite() {
  legacy_nosys_returns_error_without_halt_test();
  legacy_iopolicysys_is_version_gated_test();
}

int main() {
  return ilemu::test::run_suite("legacy_nosys", legacy_syscall_abi_suite);
}
