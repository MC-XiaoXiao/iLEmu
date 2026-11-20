// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Derive session code-cache and optional-work budgets from available
// host resources.

#pragma once

#include "foundation/host_memory.hpp"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>

namespace ilemu::runtime_detail {

struct JitCodeCacheBudget {
    std::size_t total_bytes { };
    HostMemoryBudgetSnapshot memory;
    bool explicit_override { };
};

class JitCacheActivity {
public:
    using Clock = std::chrono::steady_clock;

    void note_execution(Clock::time_point now) noexcept;
    void note_full_retirement(Clock::time_point now) noexcept;
    [[nodiscard]] Clock::duration idle_for(Clock::time_point now) const noexcept;
    [[nodiscard]] bool full_retirement_due(Clock::time_point now) const noexcept;

private:
    static constexpr Clock::duration initial_grace = std::chrono::seconds{2};
    static constexpr Clock::duration maximum_grace = std::chrono::seconds{30};
    std::optional<Clock::time_point> last_execution_;
    std::optional<Clock::time_point> last_full_retirement_;
    Clock::duration grace_ { initial_grace };
};

[[nodiscard]] JitCodeCacheBudget jit_code_cache_budget(
    std::optional<std::size_t> configured,
    const HostMemoryBudgetSnapshot& memory, std::uint64_t guest_ram_bytes);
[[nodiscard]] std::filesystem::path nearest_existing_filesystem_path(
    const std::filesystem::path& path);
[[nodiscard]] std::size_t adaptive_jit_code_cache_size(
    const HostMemoryBudgetSnapshot& memory);

} // namespace ilemu::runtime_detail
