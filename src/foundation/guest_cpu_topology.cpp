#include "ilemu/guest_cpu_topology.hpp"

namespace ilemu {
static_assert(GuestCpuTopology::single_core(400'000'000U,
    GuestCpuPerformanceClass::Legacy,
    guest_cpu_isa::armv6k | guest_cpu_isa::thumb, 1U)
        .valid());

} // namespace ilemu
