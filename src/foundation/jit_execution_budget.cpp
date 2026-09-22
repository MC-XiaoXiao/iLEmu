// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Convert host cooperation targets into adaptive guest execution
// budgets.

#include "foundation/jit_execution_budget.hpp"

#include <algorithm>

namespace ilemu {

std::uint32_t JitHostExecutionBudget::next(
    std::chrono::nanoseconds target) const noexcept
{
    if (target <= std::chrono::nanoseconds::zero())
        return 0U;

    const auto usable_nanoseconds =
        static_cast<long double>(target.count()) * 7.0L / 8.0L;
    const auto predicted = usable_nanoseconds / estimated_nanoseconds_per_tick_;
    return static_cast<std::uint32_t>(std::clamp(
        predicted, 1.0L, static_cast<long double>(maximum_ticks)));
}

void JitHostExecutionBudget::observe(
    std::uint64_t ticks_executed, std::chrono::nanoseconds elapsed) noexcept
{
    if (ticks_executed == 0U || elapsed <= std::chrono::nanoseconds::zero())
        return;

    auto sample = std::max(minimum_nanoseconds_per_tick,
        static_cast<long double>(elapsed.count()) / ticks_executed);
    // Retain bounded, asymmetric adaptation: react quickly to expensive work,
    // and increase throughput gradually after translation or a host pause.
    sample = std::clamp(sample,
        std::max(minimum_nanoseconds_per_tick,
            estimated_nanoseconds_per_tick_ / 8.0L),
        estimated_nanoseconds_per_tick_ * 8.0L);
    const auto weight = sample > estimated_nanoseconds_per_tick_ ? 2.0L : 4.0L;
    estimated_nanoseconds_per_tick_ +=
        (sample - estimated_nanoseconds_per_tick_) / weight;
}

} // namespace ilemu
