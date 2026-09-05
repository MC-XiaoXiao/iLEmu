#pragma once

#include "foundation/host_memory.hpp"
#include <cstddef>
#include <filesystem>
#include <optional>

namespace ilemu::runtime_detail {

struct JitCodeCacheBudget {
    std::size_t total_bytes { };
    HostMemoryBudgetSnapshot memory;
    bool explicit_override { };
};

[[nodiscard]] JitCodeCacheBudget jit_code_cache_budget(
    std::optional<std::size_t> configured,
    const HostMemoryBudgetSnapshot& memory);
[[nodiscard]] std::filesystem::path nearest_existing_filesystem_path(
    const std::filesystem::path& path);
[[nodiscard]] std::size_t adaptive_jit_code_cache_size(
    const HostMemoryBudgetSnapshot& memory);

} // namespace ilemu::runtime_detail
