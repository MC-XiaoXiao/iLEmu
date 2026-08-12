#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/vm_map_mig_ids.hpp"

#include "test_support.hpp"

#include "suite.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <vector>

namespace ilemu::test::mach_suite {
namespace {

using ::ilemu::test::require;

void overlapping_vm_copy_test() {
  constexpr std::uint32_t message = 0x4a000;
  constexpr std::uint32_t data_page = 0x4b000;
  constexpr std::uint32_t source = data_page + 0x20U;
  constexpr std::uint32_t destination = source + 4U;
  constexpr std::uint32_t reply_port = 0x9a0U;
  constexpr std::uint32_t request_size =
      xnu792::mig::vm_map::vm_copy_arguments[3].request_offset +
      darwin::mig_wire::word_size;
  constexpr auto request_bits = darwin::mig_wire::message_bits(
      darwin::mig_wire::disposition_copy_send,
      darwin::mig_wire::disposition_make_send_once);
  constexpr auto reply_bits = darwin::mig_wire::message_bits(
      darwin::mig_wire::disposition_move_send_once);
  constexpr std::array payload{
      std::byte{0x10}, std::byte{0x21}, std::byte{0x32}, std::byte{0x43},
      std::byte{0x54}, std::byte{0x65}, std::byte{0x76}, std::byte{0x87},
  };
  constexpr std::array expected{
      std::byte{0x10}, std::byte{0x21}, std::byte{0x32}, std::byte{0x43},
      std::byte{0x10}, std::byte{0x21}, std::byte{0x32}, std::byte{0x43},
      std::byte{0x54}, std::byte{0x65}, std::byte{0x76}, std::byte{0x87},
  };

  AddressSpace memory;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write) &&
              memory.map(data_page, AddressSpace::page_size,
                         MemoryPermission::Read | MemoryPermission::Write) &&
              memory.copy_in(source, payload),
          "vm_copy fixture setup failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};
  const auto &arguments = xnu792::mig::vm_map::vm_copy_arguments;
  const auto identifier =
      static_cast<std::uint32_t>(xnu792::mig::vm_map::Routine::vm_copy);

  require(
      memory.write32(message + darwin::mig_wire::header_bits_offset,
                     request_bits) &&
          memory.write32(message + darwin::mig_wire::header_size_offset,
                         request_size) &&
          memory.write32(message + darwin::mig_wire::header_remote_port_offset,
                         kernel.process().task_port) &&
          memory.write32(message + darwin::mig_wire::header_local_port_offset,
                         reply_port) &&
          memory.write32(message + darwin::mig_wire::header_identifier_offset,
                         identifier) &&
          memory.write32(message + arguments[1].request_offset, source) &&
          memory.write32(message + arguments[2].request_offset,
                         static_cast<std::uint32_t>(payload.size())) &&
          memory.write32(message + arguments[3].request_offset, destination),
      "vm_copy request setup failed");

  cpu.registers()[0] = message;
  cpu.registers()[1] = 3; // MACH_SEND_MSG | MACH_RCV_MSG
  cpu.registers()[2] = request_size;
  cpu.registers()[3] = AddressSpace::page_size;
  cpu.registers()[4] = 0;
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  kernel.dispatch(cpu, 0x80);

  require(
      cpu.registers()[0] == 0 &&
          memory.read32(message + darwin::mig_wire::header_bits_offset) ==
              std::optional<std::uint32_t>{reply_bits} &&
          memory.read32(message + darwin::mig_wire::header_size_offset) ==
              std::optional<std::uint32_t>{
                  darwin::mig_wire::simple_reply_payload_base} &&
          memory.read32(message +
                        darwin::mig_wire::header_remote_port_offset) ==
              std::optional<std::uint32_t>{reply_port} &&
          memory.read32(message + darwin::mig_wire::header_identifier_offset) ==
              std::optional<std::uint32_t>{identifier + 100U} &&
          memory.read32(message +
                        darwin::mig_wire::simple_request_payload_base) ==
              std::optional<std::uint32_t>{0} &&
          memory.read_bytes(source, expected.size()) ==
              std::optional<std::vector<std::byte>>{
                  std::vector<std::byte>{expected.begin(), expected.end()}},
      "vm_copy did not preserve overlap-safe XNU copy semantics");
}

} // namespace

void run_vm_copy_tests() { overlapping_vm_copy_test(); }

} // namespace ilemu::test::mach_suite
