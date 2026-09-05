#pragma once

#include "foundation/host_memory.hpp"

namespace ilemu {

[[nodiscard]] HostMemorySnapshot host_memory_snapshot();
[[nodiscard]] HostMemoryBudgetSnapshot host_memory_budget_snapshot();

} // namespace ilemu
