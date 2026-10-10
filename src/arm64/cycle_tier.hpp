/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <algorithm>
#include <cstdint>

namespace ilemu::execution::arm64 {
// Cold dynamic edges stay compact. Only observed repeated native groups earn
// bounded expansion; no guest target, memory value or host pointer is retained.
class CycleTier {
public:
    static constexpr unsigned maximum_copies = 32;
    CycleTier(bool guarded = false, bool expanded = false)
        : eligible_(guarded && !expanded)
    {
    }
    void observe(std::uint64_t ticks, std::uint64_t group_ticks,
        std::uint64_t run)
    {
        if (!eligible_ || remaining_ == 0 || group_ticks == 0)
            return;
        // The final paid prefix can be a target mismatch, not a closed group.
        // Under-count one group rather than promote frequently called returns.
        const auto groups = ticks / group_ticks;
        remaining_ -= static_cast<unsigned>(
            std::min<std::uint64_t>(remaining_, groups == 0 ? 0 : groups - 1));
        if (remaining_ == 0)
            observed_run_ = run;
    }
    bool ready(std::uint64_t run) const
    {
        return eligible_ && remaining_ == 0 && observed_run_ != run;
    }

private:
    unsigned remaining_ = 64;
    bool eligible_;
    std::uint64_t observed_run_ = 0;
};
}
