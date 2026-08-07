#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/device_profile.hpp"
#include "ilemu/guest_cpu_topology.hpp"

#include <iostream>

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

  ilemu::AddressSpace cluster_memory;
  Dynarmic::ExclusiveMonitor cluster_monitor{2};
  ilemu::CpuCluster cluster{1, 1, cluster_memory, 1,
                            ilemu::default_arm_cpu_model(), cluster_monitor,
                            1};
  if (!cluster.has_execution_resources()) {
    std::cerr << "shared monitor CpuCluster did not initialize\n";
    return 1;
  }

  return 0;
}
