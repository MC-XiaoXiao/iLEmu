/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/run.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>

namespace ilemu::execution::arm64 {
// Compile-scoped authorization. Disjoint pinned views never authorize a gap.
class CodeRegions {
public:
    static constexpr unsigned capacity = 4;
    explicit CodeRegions(InstructionRegion root)
        : CodeRegions(std::span { &root, 1 }) { }
    explicit CodeRegions(std::span<const InstructionRegion> regions)
    {
        if (regions.size() > regions_.size())
            throw std::invalid_argument("too many instruction regions");
        for (const auto& region : regions)
            if (region.begin < region.end && region.end <= (std::uint64_t { 1 } << 32U))
                regions_[count_++] = region;
        std::sort(regions_.begin(), regions_.begin() + count_,
            [](const auto& a, const auto& b) { return a.begin < b.begin; });
        unsigned merged = 0;
        for (unsigned i = 0; i < count_; ++i) {
            if (merged != 0 && regions_[i].begin <= regions_[merged - 1U].end)
                regions_[merged - 1U].end =
                    std::max(regions_[merged - 1U].end, regions_[i].end);
            else
                regions_[merged++] = regions_[i];
        }
        count_ = merged;
    }
    std::optional<InstructionRegion> containing(std::uint64_t pc) const
    {
        for (unsigned i = 0; i < count_; ++i)
            if (regions_[i].begin <= pc && pc < regions_[i].end)
                return regions_[i];
        return {};
    }

private:
    std::array<InstructionRegion, capacity> regions_ {};
    unsigned count_ = 0;
};
}
