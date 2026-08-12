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

constexpr std::uint32_t message_address = 0x51000U;
constexpr std::uint32_t reply_port = 0x930U;
constexpr std::uint32_t mach_message_trap = static_cast<std::uint32_t>(-31);
constexpr std::uint32_t mach_trap_svc = 0x80U;
constexpr std::uint32_t memory_entry_request_size = 68U;
constexpr std::uint32_t memory_entry_reply_size = 56U;
constexpr std::uint32_t vm_map_request_size = 84U;
constexpr std::uint32_t mach_vm_map_request_size = 88U;
constexpr std::uint32_t vm_map_reply_size = 40U;
constexpr std::uint32_t mach_vm_map_identifier = 4811U;
constexpr std::uint32_t map_memory_named_create = 0x0002'0000U;
constexpr std::uint32_t protection_read_write = 3U;

void dispatch_message(CompatibilityKernel &kernel, Cpu &cpu,
                      std::uint32_t send_size, std::uint32_t receive_size) {
  cpu.registers()[0] = message_address;
  cpu.registers()[1] =
      darwin::mach_message::option_send | darwin::mach_message::option_receive;
  cpu.registers()[2] = send_size;
  cpu.registers()[3] = receive_size;
  cpu.registers()[4] = reply_port;
  cpu.registers()[12] = mach_message_trap;
  kernel.dispatch(cpu, mach_trap_svc);
}

void write_memory_entry_request(AddressSpace &memory, std::uint32_t task_port) {
  const auto identifier = static_cast<std::uint32_t>(
      xnu792::mig::vm_map::Routine::mach_make_memory_entry_64);
  require(
      memory.write32(message_address,
                     darwin::mig_wire::message_bits(
                         darwin::mig_wire::disposition_copy_send,
                         darwin::mig_wire::disposition_make_send_once, true)) &&
          memory.write32(message_address + 4U, memory_entry_request_size) &&
          memory.write32(message_address + 8U, task_port) &&
          memory.write32(message_address + 12U, reply_port) &&
          memory.write32(message_address + 20U, identifier) &&
          memory.write32(message_address + 24U, 1U) &&
          memory.write32(message_address + 28U, 0U) &&
          memory.write32(message_address + 32U, 0U) &&
          memory.write32(message_address + 36U,
                         darwin::mig_wire::port_descriptor_metadata(
                             darwin::mig_wire::disposition_copy_send)) &&
          memory.write32(message_address + 40U, 0U) &&
          memory.write32(message_address + 44U, 1U) &&
          memory.write64(message_address + 48U, 2U * AddressSpace::page_size) &&
          memory.write64(message_address + 56U, 0U) &&
          memory.write32(message_address + 64U,
                         map_memory_named_create | protection_read_write),
      "mach_make_memory_entry_64 request setup failed");
}

void write_vm_map_request(AddressSpace &memory, std::uint32_t task_port,
                          std::uint32_t memory_entry,
                          std::uint32_t target_address, bool copy) {
  const auto identifier =
      static_cast<std::uint32_t>(xnu792::mig::vm_map::Routine::vm_map);
  require(
      memory.write32(message_address,
                     darwin::mig_wire::message_bits(
                         darwin::mig_wire::disposition_copy_send,
                         darwin::mig_wire::disposition_make_send_once, true)) &&
          memory.write32(message_address + 4U, vm_map_request_size) &&
          memory.write32(message_address + 8U, task_port) &&
          memory.write32(message_address + 12U, reply_port) &&
          memory.write32(message_address + 20U, identifier) &&
          memory.write32(message_address + 24U, 1U) &&
          memory.write32(message_address + 28U, memory_entry) &&
          memory.write32(message_address + 32U, 0U) &&
          memory.write32(message_address + 36U,
                         darwin::mig_wire::port_descriptor_metadata(
                             darwin::mig_wire::disposition_copy_send)) &&
          memory.write32(message_address + 40U, 0U) &&
          memory.write32(message_address + 44U, 1U) &&
          memory.write32(message_address + 48U, target_address) &&
          memory.write32(message_address + 52U, AddressSpace::page_size) &&
          memory.write32(message_address + 56U, 0U) &&
          memory.write32(message_address + 60U, 0U) &&
          memory.write32(message_address + 64U, 0U) &&
          memory.write32(message_address + 68U, copy ? 1U : 0U) &&
          memory.write32(message_address + 72U, protection_read_write) &&
          memory.write32(message_address + 76U, protection_read_write) &&
          memory.write32(message_address + 80U, 0U),
      "vm_map named-memory request setup failed");
}

void write_mach_vm_map_request(AddressSpace &memory, std::uint32_t task_port,
                               std::uint32_t memory_entry,
                               std::uint32_t target_address) {
  require(
      memory.write32(message_address,
                     darwin::mig_wire::message_bits(
                         darwin::mig_wire::disposition_copy_send,
                         darwin::mig_wire::disposition_make_send_once, true)) &&
          memory.write32(message_address + 4U, mach_vm_map_request_size) &&
          memory.write32(message_address + 8U, task_port) &&
          memory.write32(message_address + 12U, reply_port) &&
          memory.write32(message_address + 20U, mach_vm_map_identifier) &&
          memory.write32(message_address + 24U, 1U) &&
          memory.write32(message_address + 28U, memory_entry) &&
          memory.write32(message_address + 32U, 0U) &&
          memory.write32(message_address + 36U,
                         darwin::mig_wire::port_descriptor_metadata(
                             darwin::mig_wire::disposition_copy_send)) &&
          memory.write32(message_address + 40U, 0U) &&
          memory.write32(message_address + 44U, 1U) &&
          memory.write32(message_address + 48U, target_address) &&
          memory.write32(message_address + 52U, AddressSpace::page_size) &&
          memory.write32(message_address + 56U, 0U) &&
          memory.write32(message_address + 60U, 0U) &&
          memory.write64(message_address + 64U, 0U) &&
          memory.write32(message_address + 72U, 0U) &&
          memory.write32(message_address + 76U, protection_read_write) &&
          memory.write32(message_address + 80U, protection_read_write) &&
          memory.write32(message_address + 84U, 0U),
      "mach_vm_map named-memory request setup failed");
}

void named_memory_shared_and_copy_on_write_test() {
  constexpr std::uint32_t shared_address = 0x60000U;
  constexpr std::uint32_t shared_alias = 0x62000U;
  constexpr std::uint32_t private_alias = 0x64000U;
  constexpr std::uint32_t shared_value = 0x1122'3344U;
  constexpr std::uint32_t private_value = 0xaabb'ccddU;

  AddressSpace memory;
  require(memory.map(message_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "named-memory message map failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  write_memory_entry_request(memory, kernel.process().task_port);
  dispatch_message(kernel, cpu, memory_entry_request_size, 64U);
  const auto entry_name = memory.read32(message_address + 28U).value_or(0);
  require(cpu.registers()[0] == darwin::mach::success && entry_name != 0 &&
              memory.read32(message_address + 4U) ==
                  std::optional<std::uint32_t>{memory_entry_reply_size} &&
              memory.read64(message_address + 48U) ==
                  std::optional<std::uint64_t>{2U * AddressSpace::page_size},
          "mach_make_memory_entry_64 did not return a named object");

  for (const auto address : {shared_address, shared_alias}) {
    write_vm_map_request(memory, kernel.process().task_port, entry_name,
                         address, false);
    dispatch_message(kernel, cpu, vm_map_request_size, 48U);
    require(cpu.registers()[0] == darwin::mach::success &&
                memory.read32(message_address + 4U) ==
                    std::optional<std::uint32_t>{vm_map_reply_size} &&
                memory.read32(message_address + 32U) ==
                    std::optional<std::uint32_t>{darwin::mach::success} &&
                memory.mapped(address, AddressSpace::page_size),
            "vm_map did not map the named-memory object");
  }
  require(memory.write32(shared_address, shared_value) &&
              memory.read32(shared_alias) ==
                  std::optional<std::uint32_t>{shared_value},
          "shared named-memory mappings did not observe the same write");

  write_vm_map_request(memory, kernel.process().task_port, entry_name,
                       private_alias, true);
  dispatch_message(kernel, cpu, vm_map_request_size, 48U);
  require(memory.read32(private_alias) ==
                  std::optional<std::uint32_t>{shared_value} &&
              memory.write32(private_alias, private_value) &&
              memory.read32(shared_address) ==
                  std::optional<std::uint32_t>{shared_value},
          "vm_map(copy=TRUE) did not preserve copy-on-write isolation");

  constexpr std::uint32_t mach_vm_alias = 0x66000U;
  write_mach_vm_map_request(memory, kernel.process().task_port, entry_name,
                            mach_vm_alias);
  dispatch_message(kernel, cpu, mach_vm_map_request_size, 48U);
  require(cpu.registers()[0] == darwin::mach::success &&
              memory.read32(message_address + 4U) ==
                  std::optional<std::uint32_t>{vm_map_reply_size} &&
              memory.read32(message_address + 32U) ==
                  std::optional<std::uint32_t>{darwin::mach::success} &&
              memory.read32(mach_vm_alias) ==
                  std::optional<std::uint32_t>{shared_value},
          "Darwin 8 mach_vm_map alias did not map named memory");
}

} // namespace

void run_vm_memory_entry_tests() {
  named_memory_shared_and_copy_on_write_test();
}

} // namespace ilemu::test::mach_suite
