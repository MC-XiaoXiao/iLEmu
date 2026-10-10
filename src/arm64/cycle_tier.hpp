/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <algorithm>
#include <cstdint>

namespace ilemu::execution::arm64 {
enum class CycleExpansion { Compact, Untracked, Hot };

// Cold cycles stay compact. Only observed repeated native groups earn
// bounded expansion; no guest target, memory value or host pointer is retained.
class CycleTier {
public:
    static constexpr unsigned maximum_copies = 64;
    // Guarded copies repeat target/ISA checks and cost more native code.
    static constexpr unsigned guarded_copies = 32;
    static constexpr unsigned natural_copies(CycleExpansion expansion)
    {
        // Views without an epoch cannot retain heat across code leases.
        return expansion == CycleExpansion::Compact ? 1U
             : expansion == CycleExpansion::Untracked ? 32U : maximum_copies;
    }
    CycleTier(bool closed = false, bool expanded = false)
        : eligible_(closed && !expanded)
    {
    }
    void observe(std::uint64_t ticks, std::uint64_t group_ticks)
    {
        if (!eligible_ || remaining_ == 0 || group_ticks == 0)
            return;
        // The final paid prefix can be a target mismatch, not a closed group.
        // Under-count one group rather than promote frequently called returns.
        const auto groups = ticks / group_ticks;
        remaining_ -= static_cast<unsigned>(
            std::min<std::uint64_t>(remaining_, groups == 0 ? 0 : groups - 1));
    }
    bool ready() const
    {
        return eligible_ && remaining_ == 0;
    }

private:
    unsigned remaining_ = 64;
    bool eligible_;
};
}
