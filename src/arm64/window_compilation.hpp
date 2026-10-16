/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "address_windows.hpp"
#include "cycle_formation.hpp"
#include <optional>
#include <utility>

namespace ilemu::execution::arm64 {
// One compile-scoped owner of the window choices. The supplied lowering keeps
// the entry, pinned views and immutable source policies fixed for every body.
template<typename Compile>
class WindowCompilation final : public CycleFormation {
public:
    WindowCompilation(const AddressWindows& windows, const TracePath& path, Compile compile)
        : windows_(windows), path_(path), compile_(std::move(compile)) { }
    CompiledTrace select(CompiledTrace slow)
    {
        if (completed_)
            return slow;
        auto full = windows_;
        full.expand(slow.instructions);
        if (full.empty())
            return slow;
        prepare_fast(slow, full);
        if (!path_ && !attempted(slow) && full.can_select(*fast_, slow))
            if (auto selected = build(slow))
                return std::move(*selected);
        return full.select(std::move(*fast_), std::move(slow));
    }

private:
    bool attempted(const TraceMetadata& group) const
    {
        return formed_ && formed_->selectable_with(group);
    }
    void prepare_fast(const TraceMetadata& group, const AddressWindows& windows)
    {
        if (!fast_ || !fast_->selectable_with(group))
            fast_.emplace(compile_(&windows, group.instructions, nullptr));
    }
    std::optional<CompiledTrace> build(const TraceMetadata& group) override
    {
        const auto limit = windows_.fallback_limit(group.instructions);
        if (path_ || !group.closed || !windows_.advances() || limit >= group.instructions)
            return {};
        formed_ = group;
        auto full = windows_;
        full.expand(group.instructions);
        if (full.empty())
            return {};
        prepare_fast(group, full);
        if (!fast_->selectable_with(group))
            return {};
        auto slow = compile_(nullptr, limit, nullptr);
        auto tail = windows_;
        tail.expand(slow.instructions);
        if (tail.empty())
            return {};
        auto fast = compile_(&tail, slow.instructions, &full);
        if (!tail.can_select(fast, slow))
            return {};
        auto bounded = tail.select(std::move(fast), std::move(slow));
        if (!full.can_select(*fast_, bounded))
            return {};
        completed_ = true;
        return full.select(std::move(*fast_), std::move(bounded), false);
    }
    const AddressWindows& windows_;
    const TracePath& path_;
    Compile compile_;
    std::optional<TraceMetadata> formed_;
    std::optional<CompiledTrace> fast_;
    bool completed_ = false;
};
}
