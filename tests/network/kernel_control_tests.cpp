#include "suite.hpp"

#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kernel_control_abi.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/kernel_control.hpp"
#include "ilemu/output.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>

namespace ilemu::test::network_suite {
namespace {

using ::ilemu::test::require;

void commcenter_kernel_control_contract_test() {
  AddressSpace memory;
  constexpr std::uint32_t base = 0x51000;
  constexpr std::uint32_t control_info = base;
  constexpr std::uint32_t socket_address = base + 0x100;
  constexpr std::uint32_t peer_address = base + 0x180;
  constexpr std::uint32_t peer_size = base + 0x1c0;
  constexpr std::uint32_t non_block = base + 0x1d0;
  constexpr std::uint32_t payload = base + 0x1e0;
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "kernel-control memory map failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};
  const auto invoke = [&](std::uint32_t syscall) {
    cpu.registers()[12] = syscall;
    kernel.dispatch(cpu, 0x80);
  };

  cpu.registers()[0] = darwin::kernel_control::protocol_family_system;
  cpu.registers()[1] = darwin::socket::datagram;
  cpu.registers()[2] = darwin::kernel_control::protocol_control;
  invoke(darwin::syscall::socket);
  const auto fd = cpu.registers()[0];
  require(fd >= 3, "PF_SYSTEM kernel-control socket creation failed");

  require(memory.copy_in(
              control_info +
                  darwin::kernel_control::control_info_name_offset,
              std::as_bytes(std::span{
                  bsd::kernel_control::ip_interface_name.data(),
                  bsd::kernel_control::ip_interface_name.size() + 1})) &&
              memory.write32(non_block, 1),
          "kernel-control lookup input write failed");
  cpu.registers()[0] = fd;
  cpu.registers()[1] = darwin::socket::ioctl_non_block;
  cpu.registers()[2] = non_block;
  invoke(54); // ioctl
  require(cpu.registers()[0] == 0,
          "FIONBIO failed on kernel-control socket");

  cpu.registers()[0] = fd;
  cpu.registers()[1] = darwin::kernel_control::ioctl_get_info;
  cpu.registers()[2] = control_info;
  invoke(54); // ioctl
  const auto identifier = memory.read32(
      control_info +
      darwin::kernel_control::control_info_identifier_offset);
  require(cpu.registers()[0] == 0 &&
              identifier ==
                  std::optional<std::uint32_t>{
                      bsd::kernel_control::ip_interface_identifier},
          "CTLIOCGINFO did not resolve com.apple.ipif");

  require(
      memory.write8(
          socket_address +
              darwin::kernel_control::socket_address_length_offset,
          darwin::kernel_control::socket_address_size) &&
          memory.write8(
              socket_address +
                  darwin::kernel_control::socket_address_family_offset,
              darwin::kernel_control::protocol_family_system) &&
          memory.write16(
              socket_address +
                  darwin::kernel_control::socket_address_system_offset,
              darwin::kernel_control::system_address_control) &&
          memory.write32(
              socket_address +
                  darwin::kernel_control::socket_address_identifier_offset,
              *identifier) &&
          memory.write32(
              socket_address +
                  darwin::kernel_control::socket_address_unit_offset,
              0),
      "sockaddr_ctl input write failed");
  cpu.registers()[0] = fd;
  cpu.registers()[1] = socket_address;
  cpu.registers()[2] = darwin::kernel_control::socket_address_size;
  invoke(darwin::syscall::connect);
  require(cpu.registers()[0] == 0, "kernel-control connect failed");

  require(memory.write32(peer_size,
                         darwin::kernel_control::socket_address_size),
          "kernel-control peer capacity write failed");
  cpu.registers()[0] = fd;
  cpu.registers()[1] = peer_address;
  cpu.registers()[2] = peer_size;
  invoke(darwin::syscall::get_peer_name);
  require(
      cpu.registers()[0] == 0 &&
          memory.read8(
              peer_address +
              darwin::kernel_control::socket_address_family_offset) ==
              std::optional<std::uint8_t>{
                  darwin::kernel_control::protocol_family_system} &&
          memory.read32(
              peer_address +
              darwin::kernel_control::socket_address_identifier_offset) ==
              identifier &&
          memory.read32(
              peer_address +
              darwin::kernel_control::socket_address_unit_offset) ==
              std::optional<std::uint32_t>{0},
      "getpeername did not return the connected sockaddr_ctl");

  require(memory.write32(payload, 0x12345678),
          "kernel-control payload write failed");
  cpu.registers()[0] = fd;
  cpu.registers()[1] = payload;
  cpu.registers()[2] = sizeof(std::uint32_t);
  invoke(darwin::syscall::write);
  require(cpu.registers()[0] == sizeof(std::uint32_t),
          "connected kernel-control write failed");
}

} // namespace

void run_kernel_control_tests() {
  commcenter_kernel_control_contract_test();
}

} // namespace ilemu::test::network_suite
