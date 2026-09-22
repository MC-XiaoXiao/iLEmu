// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Convert host cooperation targets into adaptive guest execution
// budgets.

#pragma once

#include <chrono>
#include <cstdint>

namespace ilemu {

// Converts wall-time cooperation targets to the existing Guest cycle budget.
// Measuring ticks rather than adding a second per-block countdown keeps hot
// native execution cheap; consumed Guest ticks retain their original meaning.
class JitHostExecutionBudget {
public:
    [[nodiscard]] std::uint32_t next(
        std::chrono::nanoseconds target) const noexcept;

    void observe(std::uint64_t ticks_executed,
        std::chrono::nanoseconds elapsed) noexcept;

private:
    static constexpr std::uint32_t maximum_ticks = 16'777'216U;
    static constexpr long double minimum_nanoseconds_per_tick = 1.0L / 1024.0L;
    // Conservative during cold translation; hot observations converge without
    // rounding sub-nanosecond instruction costs to a whole nanosecond.
    long double estimated_nanoseconds_per_tick_ { 500.0L };
};

} // namespace ilemu
