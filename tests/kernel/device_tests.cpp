#include "suite.hpp"

#include "test_support.hpp"

#include "ilemu/address_space.hpp"
#include "ilemu/baseband_device.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_tty_abi.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/output.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string_view>

namespace ilemu::test::kernel {
namespace {

using ::ilemu::test::require;

void random_character_device_test() {
  AddressSpace memory;
  constexpr std::uint32_t base = 0x5b000U;
  constexpr std::uint32_t path_address = base;
  constexpr std::uint32_t stat_address = base + 0x100U;
  constexpr std::uint32_t random_address = base + 0x200U;
  constexpr std::string_view path = "/dev/srandom";
  std::array<std::byte, path.size() + 1> path_bytes{};
  for (std::size_t index = 0; index < path.size(); ++index) {
    path_bytes[index] = static_cast<std::byte>(path[index]);
  }
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write) &&
              memory.copy_in(path_address, path_bytes),
          "random-device fixture setup failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output, "build/rootfs"};

  cpu.registers()[0] = path_address;
  cpu.registers()[1] = darwin::open_flag::read_only;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = darwin::syscall::open;
  kernel.dispatch(cpu, 0x80);
  const auto descriptor = cpu.registers()[0];
  require(descriptor >= 3, "/dev/srandom did not open as a virtual device");

  cpu.registers()[0] = descriptor;
  cpu.registers()[1] = stat_address;
  cpu.registers()[12] = 189;
  kernel.dispatch(cpu, 0x80);
  constexpr std::uint32_t file_type_mask = 0170000U;
  constexpr std::uint32_t character_device_type = 0020000U;
  require(cpu.registers()[0] == 0 &&
              (memory.read16(stat_address + 8).value_or(0) & file_type_mask) ==
                  character_device_type,
          "fstat did not identify the entropy source as a character device");

  cpu.registers()[0] = descriptor;
  cpu.registers()[1] = random_address;
  cpu.registers()[2] = 32;
  cpu.registers()[12] = darwin::syscall::read;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 32 && memory.read_bytes(random_address, 32) !=
                                          std::optional<std::vector<std::byte>>{
                                              std::vector<std::byte>(32)},
          "virtual entropy source did not return random bytes");
}

void baseband_h5_transport_test() {
  bsd::baseband_device::State state;
  require(!state.h5_transport_mode(),
          "baseband H5 transport must start disabled");
  state.set_h5_transport_mode(true);
  require(state.h5_transport_mode(),
          "baseband H5 transport state did not enable");

  AddressSpace memory;
  constexpr std::uint32_t base = 0x5c000U;
  constexpr std::uint32_t path_address = base;
  constexpr std::uint32_t argument_address = base + 0x100U;
  constexpr std::uint32_t read_address = base + 0x200U;
  constexpr std::uint32_t write_address = base + 0x300U;
  constexpr auto path = bsd::baseband_device::path;
  std::array<std::byte, path.size() + 1> path_bytes{};
  for (std::size_t index = 0; index < path.size(); ++index) {
    path_bytes[index] = static_cast<std::byte>(path[index]);
  }
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write) &&
              memory.copy_in(path_address, path_bytes) &&
              memory.write32(argument_address, 1),
          "baseband H5 fixture setup failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output, "build/rootfs"};

  cpu.registers()[0] = path_address;
  cpu.registers()[1] = darwin::open_flag::read_write;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = darwin::syscall::open;
  kernel.dispatch(cpu, 0x80);
  const auto descriptor = cpu.registers()[0];
  require(descriptor >= 3, "/dev/h5.baseband did not open");

  cpu.registers()[0] = descriptor;
  cpu.registers()[1] = darwin::tty::set_h5_transport_mode;
  cpu.registers()[2] = argument_address;
  cpu.registers()[12] = darwin::syscall::ioctl;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              stream.str().find("IOAOSH5 enabled=1") != std::string::npos,
          "IOAOSH5 did not enable the baseband H5 transport");

  const std::array inbound{std::byte{0x01}, std::byte{0x7e},
                           std::byte{0xa5}};
  kernel.enqueue_baseband_input(inbound);
  cpu.registers()[0] = descriptor;
  cpu.registers()[1] = read_address;
  cpu.registers()[2] = 2;
  cpu.registers()[12] = darwin::syscall::read;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 2 &&
              memory.read_bytes(read_address, 2) ==
                  std::optional<std::vector<std::byte>>{
                      std::vector<std::byte>{inbound[0], inbound[1]}},
          "baseband replay bytes did not reach the guest");

  const std::array outbound{std::byte{0xf9}, std::byte{0x3f}};
  require(memory.copy_in(write_address, outbound),
          "baseband capture fixture setup failed");
  cpu.registers()[0] = descriptor;
  cpu.registers()[1] = write_address;
  cpu.registers()[2] = static_cast<std::uint32_t>(outbound.size());
  cpu.registers()[12] = darwin::syscall::write;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == outbound.size() &&
              kernel.take_baseband_output() ==
                  std::vector<std::byte>{outbound.begin(), outbound.end()},
          "guest baseband write was not captured");
}

} // namespace

void run_device_tests() {
  random_character_device_test();
  baseband_h5_transport_test();
}

} // namespace ilemu::test::kernel
