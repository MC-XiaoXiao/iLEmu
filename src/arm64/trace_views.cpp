/* SPDX-License-Identifier: MPL-2.0 */
#include "trace_views.hpp"
namespace ilemu::execution::arm64 {
namespace {
    bool same(InstructionRegion a, InstructionRegion b)
    {
        return a.begin == b.begin && a.end == b.end;
    }
}
std::unique_ptr<TraceViews> TraceViews::capture(
    InstructionLease& lease, InstructionRegion root, std::uint32_t pc)
{
    constexpr auto limit = std::uint64_t { 1 } << 32U;
    const auto prepared = lease.continuation_region();
    if (!prepared || !same(*prepared, root) || root.end > limit ||
        root.begin > pc || std::uint64_t { pc } + 4U > root.end)
        return {};
    const auto primary = lease.cached_continuation(pc);
    if (!primary || !primary->inspectable || !primary->identity || !primary->generation ||
        !same(primary->region, root))
        return {};
    TraceViews result;
    result.views_[result.count_++] = { *primary, pc };
    result.region_ = root;
    bool forward = true, backward = true;
    while (result.count_ < result.views_.size() && (forward || backward)) {
        const auto extend = [&](bool ahead) {
            auto& available = ahead ? forward : backward;
            if (!available || result.count_ == result.views_.size())
                return;
            if ((ahead && result.region_.end == limit) ||
                (!ahead && result.region_.begin < 4U)) {
                available = false;
                return;
            }
            const auto probe = static_cast<std::uint32_t>(
                ahead ? result.region_.end : result.region_.begin - 4U);
            const auto next = lease.cached_continuation(probe);
            if (!next || !next->inspectable || !next->identity || !next->generation ||
                next->region.begin >= next->region.end ||
                next->region.end > limit || next->region.begin > probe ||
                std::uint64_t { probe } + 4U > next->region.end ||
                (ahead ? next->region.begin != result.region_.end
                       : next->region.end != result.region_.begin)) {
                available = false;
                return;
            }
            result.views_[result.count_++] = { *next, probe };
            if (ahead)
                result.region_.end = next->region.end;
            else
                result.region_.begin = next->region.begin;
        };
        extend(true);
        extend(false);
    }
    return result.count_ > 1 ? std::make_unique<TraceViews>(result) : nullptr;
}
bool TraceViews::pin(InstructionLease& lease, InstructionRegion root,
    EntryAuthorization authorization) const noexcept
{
    if (!same(root, views_[0].view.region))
        return false;
    if (authorization == EntryAuthorization::Prepared) {
        const auto prepared = lease.continuation_region();
        if (!prepared || !same(*prepared, root))
            return false;
    }
    for (unsigned i = 0; i < count_; ++i) {
        const auto& expected = views_[i];
        const auto current = lease.cached_continuation(expected.probe);
        if (!current || !current->inspectable || current->identity != expected.view.identity ||
            current->generation != expected.view.generation ||
            !same(current->region, expected.view.region))
            return false;
    }
    return true;
}
}
