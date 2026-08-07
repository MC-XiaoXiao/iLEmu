#include "ilemu/device_profile.hpp"
#include "ilemu/guest_cpu_topology.hpp"

#include <iostream>

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

  return 0;
}
