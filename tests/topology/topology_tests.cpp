#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/device_profile.hpp"
#include "ilemu/guest_cpu_topology.hpp"

#include <array>
#include <iostream>
#include <optional>
#include <span>

#include <dynarmic/interface/exclusive_monitor.h>

int main() {
  for (const auto &profile : ilemu::DeviceProfile::available_profiles()) {
    const auto &topology = profile.guest_cpu_topology;
    if (!topology.valid() ||
        topology.logical_cpu_count != topology.physical_core_count ||
        topology.cluster_count != 1U ||
        topology.clusters[0].affinity_mask != 1U ||
        topology.clusters[0].frequency_hz != profile.cpu_hz) {
      std::cerr << "invalid faithful topology for " << profile.product_type
                << '\n';
      return 1;
    }
  }

  auto invalid = ilemu::GuestCpuTopology::single_core(
      400'000'000U, ilemu::GuestCpuPerformanceClass::Legacy,
      ilemu::guest_cpu_isa::armv6k | ilemu::guest_cpu_isa::thumb, 1U);
  invalid.clusters[0].affinity_mask = 2U;
  if (invalid.valid()) {
    std::cerr << "invalid affinity mask accepted\n";
    return 1;
  }

  ilemu::AddressSpace first_memory;
  ilemu::AddressSpace second_memory;
  if (!first_memory.map(0x1000U, ilemu::AddressSpace::page_size,
                        ilemu::MemoryPermission::Read |
                            ilemu::MemoryPermission::Write) ||
      !second_memory.map(0x1000U, ilemu::AddressSpace::page_size,
                         ilemu::MemoryPermission::Read |
                             ilemu::MemoryPermission::Write)) {
    std::cerr << "exclusive monitor test mapping failed\n";
    return 1;
  }
  Dynarmic::ExclusiveMonitor monitor{2};
  first_memory.set_exclusive_write_observer([&monitor] { monitor.Clear(); });
  second_memory.set_exclusive_write_observer([&monitor] { monitor.Clear(); });
  first_memory.write32(0x1000U, 1U);
  static_cast<void>(monitor.ReadAndMark<std::uint32_t>(
      0, 0x1000U, [&] { return first_memory.read32(0x1000U).value_or(0U); }));
  if (!second_memory.write32(0x1000U, 2U) ||
      monitor.DoExclusiveOperation<std::uint32_t>(
          0, 0x1000U, [](std::uint32_t) { return true; })) {
    std::cerr << "shared write did not invalidate exclusive reservation\n";
    return 1;
  }

  ilemu::AddressSpace shared_source;
  ilemu::AddressSpace shared_alias;
  if (!shared_source.map(0x2000U, ilemu::AddressSpace::page_size,
                         ilemu::MemoryPermission::Read |
                             ilemu::MemoryPermission::Write) ||
      !shared_source.write32(0x2000U, 3U)) {
    std::cerr << "shared backing source setup failed\n";
    return 1;
  }
  const auto shared_backings =
      shared_source.share_pages(0x2000U, ilemu::AddressSpace::page_size);
  if (!shared_backings ||
      !shared_alias.map_page_backings(
          0x3000U, ilemu::AddressSpace::page_size,
          ilemu::MemoryPermission::Read | ilemu::MemoryPermission::Write,
          *shared_backings, ilemu::AddressSpace::PageMappingMode::Shared)) {
    std::cerr << "shared backing alias setup failed\n";
    return 1;
  }
  if (!shared_source.map_page_backings(
          0x3000U, ilemu::AddressSpace::page_size,
          ilemu::MemoryPermission::Read | ilemu::MemoryPermission::Write,
          *shared_backings, ilemu::AddressSpace::PageMappingMode::Shared)) {
    std::cerr << "same-address-space alias setup failed\n";
    return 1;
  }
  shared_alias.set_parallel_access(false);
  auto **shared_write_table = shared_alias.jit_write_page_table();
  if (shared_write_table == nullptr ||
      shared_write_table[0x3000U / ilemu::AddressSpace::page_size] != nullptr) {
    std::cerr << "shared backing retained a direct JIT write pointer\n";
    return 1;
  }
  shared_source.set_exclusive_write_observer([&monitor] { monitor.Clear(); });
  shared_alias.set_exclusive_write_observer([&monitor] { monitor.Clear(); });
  static_cast<void>(monitor.ReadAndMark<std::uint32_t>(
      0, 0x2000U, [&] { return shared_source.read32(0x2000U).value_or(0U); }));
  if (!shared_alias.write32(0x3000U, 4U) ||
      monitor.DoExclusiveOperation<std::uint32_t>(
          0, 0x2000U, [](std::uint32_t) { return true; })) {
    std::cerr << "shared alias write bypassed reservation invalidation\n";
    return 1;
  }

  Dynarmic::ExclusiveMonitor identity_monitor{2};
  auto address_resolver =
      std::make_shared<ilemu::GuestExclusiveAddressResolver>();
  address_resolver->bind(0, 1, shared_source);
  identity_monitor.SetAddressResolver(
      &ilemu::GuestExclusiveAddressResolver::resolve_callback,
      address_resolver.get());
  static_cast<void>(identity_monitor.ReadAndMark<std::uint32_t>(
      0, 0x2000U,
      [&] { return shared_source.read32(0x2000U).value_or(0U); }));
  if (!identity_monitor.DoExclusiveOperation<std::uint32_t>(
          0, 0x3000U, [](std::uint32_t) { return true; })) {
    std::cerr << "shared aliases did not resolve to one reservation key\n";
    return 1;
  }
  static_cast<void>(identity_monitor.ReadAndMark<std::uint32_t>(
      0, 0x2000U,
      [&] { return shared_source.read32(0x2000U).value_or(0U); }));
  if (!shared_alias.write32(0x3000U, 8U) ||
      identity_monitor.DoExclusiveOperation<std::uint32_t>(
          0, 0x2000U, [](std::uint32_t) { return true; })) {
    std::cerr << "cross-address-space shared write kept a stale reservation\n";
    return 1;
  }

  ilemu::AddressSpace cow_source;
  if (!cow_source.map(0x4000U, ilemu::AddressSpace::page_size,
                      ilemu::MemoryPermission::Read |
                          ilemu::MemoryPermission::Write) ||
      !cow_source.write32(0x4000U, 7U)) {
    std::cerr << "copy-on-write source setup failed\n";
    return 1;
  }
  auto copy_on_write = cow_source.clone();
  if (!copy_on_write || !copy_on_write->write32(0x4000U, 5U)) {
    std::cerr << "copy-on-write reservation setup failed\n";
    return 1;
  }
  address_resolver->unbind(0, 1, shared_source);
  address_resolver->bind(0, 1, cow_source);
  address_resolver->bind(1, 1, *copy_on_write);
  static_cast<void>(identity_monitor.ReadAndMark<std::uint32_t>(
      0, 0x4000U,
      [&] { return cow_source.read32(0x4000U).value_or(0U); }));
  if (!identity_monitor.DoExclusiveOperation<std::uint32_t>(
          0, 0x4000U, [](std::uint32_t) { return true; })) {
    std::cerr << "copy-on-write changed the original reservation identity\n";
    return 1;
  }

  static_cast<void>(identity_monitor.ReadAndMark<std::uint32_t>(
      0, 0x4000U,
      [&] { return cow_source.read32(0x4000U).value_or(0U); }));
  if (!cow_source.unmap(0x4000U, ilemu::AddressSpace::page_size) ||
      !cow_source.map(0x4000U, ilemu::AddressSpace::page_size,
                         ilemu::MemoryPermission::Read |
                             ilemu::MemoryPermission::Write) ||
      !cow_source.write32(0x4000U, 6U) ||
      identity_monitor.DoExclusiveOperation<std::uint32_t>(
          0, 0x4000U, [](std::uint32_t) { return true; })) {
    std::cerr << "unmap/remap reused an old reservation identity\n";
    return 1;
  }

  ilemu::AddressSpace cluster_memory;
  Dynarmic::ExclusiveMonitor cluster_monitor{2};
  ilemu::CpuCluster cluster{1, 1, cluster_memory, 1,
                            ilemu::default_arm_cpu_model(), cluster_monitor,
                            1};
  if (!cluster.has_execution_resources()) {
    std::cerr << "shared monitor CpuCluster did not initialize\n";
    return 1;
  }

  ilemu::AddressSpace direct_memory;
  constexpr std::uint32_t direct_code = 0x6000U;
  constexpr std::uint32_t direct_data = 0x7000U;
  if (!direct_memory.map(
          direct_code, ilemu::AddressSpace::page_size,
          ilemu::MemoryPermission::Read | ilemu::MemoryPermission::Write |
              ilemu::MemoryPermission::Execute) ||
      !direct_memory.map(
          direct_data, ilemu::AddressSpace::page_size,
          ilemu::MemoryPermission::Read | ilemu::MemoryPermission::Write) ||
      !direct_memory.write32(direct_data, 1U)) {
    std::cerr << "single-core direct-write mapping setup failed\n";
    return 1;
  }
  direct_memory.set_parallel_access(false);
  const std::array<std::uint32_t, 6> direct_code_words{
      0xe1921f9fU, // ldrex r1, [r2]
      0xe3a03002U, // mov r3, #2
      0xe5823000U, // str r3, [r2]
      0xe1820f93U, // strex r0, r3, [r2]
      0xef000080U, // svc #0x80
      0xe1a00000U, // nop
  };
  if (!direct_memory.copy_in(
          direct_code,
          std::as_bytes(std::span{direct_code_words}))) {
    std::cerr << "single-core direct-write code setup failed\n";
    return 1;
  }
  Dynarmic::ExclusiveMonitor direct_monitor{1};
  auto direct_resolver =
      std::make_shared<ilemu::GuestExclusiveAddressResolver>();
  ilemu::CpuCluster direct_cluster{
      1, 1, direct_memory, 1, ilemu::default_arm_cpu_model(), direct_monitor,
      0, {}, direct_resolver};
  auto **direct_write_table = direct_memory.jit_write_page_table();
  if (direct_write_table == nullptr ||
      direct_write_table[direct_data / ilemu::AddressSpace::page_size] ==
          nullptr) {
    std::cerr << "single-core direct-write page table was disabled\n";
    return 1;
  }
  auto &direct_cpu = direct_cluster.cpu(0);
  direct_cpu.registers()[2] = direct_data;
  direct_cpu.registers()[15] = direct_code;
  direct_cpu.set_cpsr(0x10U);
  const auto direct_result = direct_cpu.run(64);
  if (direct_result.svc != std::optional<std::uint32_t>{0x80U} ||
      direct_cpu.registers()[0] != 1U ||
      direct_memory.read32(direct_data) !=
          std::optional<std::uint32_t>{2U} ||
      direct_write_table[direct_data / ilemu::AddressSpace::page_size] !=
          nullptr) {
    std::cerr << "single-core direct-write reservation tracking failed\n";
    return 1;
  }

  return 0;
}
