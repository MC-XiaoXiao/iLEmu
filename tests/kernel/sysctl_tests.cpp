#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_sysctl.hpp"
#include "ilemu/device_profile.hpp"
#include "ilemu/kernel.hpp"

#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace ilemu::test::kernel {

void run_sysctl_tests() {
  AddressSpace memory;
  constexpr std::uint32_t base = 0x5a000;
  constexpr std::uint32_t request_mib = base;
  constexpr std::uint32_t result_mib = base + 0x20;
  constexpr std::uint32_t result_size = base + 0x40;
  constexpr std::uint32_t name_address = base + 0x60;
  constexpr std::uint32_t value_address = base + 0x80;
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "sysctl test memory map failed");

  constexpr std::string_view name = "hw.machine";
  std::array<std::byte, name.size()> name_bytes{};
  for (std::size_t index = 0; index < name.size(); ++index) {
    name_bytes[index] = static_cast<std::byte>(name[index]);
  }
  require(memory.write32(request_mib, darwin::sysctl::control_unspecified) &&
              memory.write32(request_mib + 4,
                             darwin::sysctl::operation_name_to_oid) &&
              memory.write32(result_size, 8) &&
              memory.copy_in(name_address, name_bytes),
          "sysctl.name2oid fixture setup failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  const auto &profile = DeviceProfile::default_profile();
  CompatibilityKernel kernel{memory, output, {}, profile};
  cpu.registers()[0] = request_mib;
  cpu.registers()[1] = 2;
  cpu.registers()[2] = result_mib;
  cpu.registers()[3] = result_size;
  cpu.registers()[4] = name_address;
  cpu.registers()[5] = static_cast<std::uint32_t>(name.size());
  cpu.registers()[12] = 202;
  kernel.dispatch(cpu, 0x80);
  require(
      cpu.registers()[0] == 0 &&
          memory.read32(result_mib) ==
              std::optional<std::uint32_t>{darwin::sysctl::control_hardware} &&
          memory.read32(result_mib + 4) ==
              std::optional<std::uint32_t>{darwin::sysctl::hardware_machine},
      "sysctl.name2oid did not resolve hw.machine");

  require(memory.write32(result_size, 32),
          "hw.machine result size setup failed");
  cpu.registers()[0] = result_mib;
  cpu.registers()[1] = 2;
  cpu.registers()[2] = value_address;
  cpu.registers()[3] = result_size;
  cpu.registers()[4] = 0;
  cpu.registers()[5] = 0;
  cpu.registers()[12] = 202;
  kernel.dispatch(cpu, 0x80);
  const auto value =
      memory.read_bytes(value_address, profile.product_type.size() + 1);
  require(cpu.registers()[0] == 0 && value &&
              std::string_view{reinterpret_cast<const char *>(value->data())} ==
                  profile.product_type,
          "HW_MACHINE did not project the selected device identifier");

  const auto model_identifier = darwin::sysctl::resolve_name("hw.model");
  require(model_identifier && model_identifier->size == 2 &&
              model_identifier->components[0] ==
                  darwin::sysctl::control_hardware &&
              model_identifier->components[1] == darwin::sysctl::hardware_model,
          "sysctl.name2oid did not resolve hw.model");
  require(memory.write32(result_mib, darwin::sysctl::control_hardware) &&
              memory.write32(result_mib + 4, darwin::sysctl::hardware_model) &&
              memory.write32(result_size, 32),
          "hw.model fixture setup failed");
  cpu.registers()[0] = result_mib;
  cpu.registers()[1] = 2;
  cpu.registers()[2] = value_address;
  cpu.registers()[3] = result_size;
  cpu.registers()[4] = 0;
  cpu.registers()[5] = 0;
  cpu.registers()[12] = 202;
  kernel.dispatch(cpu, 0x80);
  const auto model =
      memory.read_bytes(value_address, profile.board_config.size() + 1);
  require(cpu.registers()[0] == 0 && model &&
              std::string_view{reinterpret_cast<const char *>(model->data())} ==
                  profile.board_config,
          "HW_MODEL did not project the selected platform identifier");

  require(memory.write32(request_mib, darwin::sysctl::control_kernel) &&
              memory.write32(request_mib + 4,
                             darwin::sysctl::kernel_process_arguments) &&
              memory.write32(request_mib + 8, kernel.process().pid) &&
              memory.write32(result_size, 256),
          "KERN_PROCARGS fixture setup failed");
  cpu.registers()[0] = request_mib;
  cpu.registers()[1] = 3;
  cpu.registers()[2] = value_address;
  cpu.registers()[3] = result_size;
  cpu.registers()[4] = 0;
  cpu.registers()[5] = 0;
  cpu.registers()[12] = 202;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              memory.read_c_string(value_address) ==
                  std::optional<std::string>{"/sbin/launchd"} &&
              memory.read32(result_size).value_or(0) >
                  std::string_view{"/sbin/launchd"}.size(),
          "KERN_PROCARGS did not export the target process image and argv");
}

} // namespace ilemu::test::kernel
