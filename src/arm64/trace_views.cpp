/* SPDX-License-Identifier: MPL-2.0 */
#include "trace_views.hpp"
#include "instruction_window.hpp"
#include "dynamic_backedge.hpp"
#include "arm/architectural_stop.hpp"
#include <memory_resource>
namespace ilemu::execution::arm64 {
namespace {
    bool same(InstructionRegion a, InstructionRegion b)
    {
        return a.begin == b.begin && a.end == b.end;
    }
}
std::unique_ptr<TraceViews> TraceViews::capture(
    InstructionSource& source, InstructionLease& lease, InstructionRegion root,
    std::uint32_t pc, bool thumb, unsigned it)
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
    // Follow the compiler's taken direct path before spending the remaining
    // pin budget on neighbours. Calls may cross unrelated or unmapped pages.
    const auto start = pc;
    const auto start_thumb = thumb;
    const auto start_it = it;
    std::pmr::monotonic_buffer_resource scratch;
    InstructionWindow window(source, root, thumb, &scratch);
    std::array<std::uint64_t, 256> visited;
    for (unsigned n = 0; n < visited.size(); ++n) {
        const auto location = (std::uint64_t { pc } << 9U) | (it << 1U) | thumb;
        // The compiler follows this deterministic taken path. A cycle that
        // excludes the entry cannot become a closed entry by adding views.
        const auto end = visited.begin() + n;
        if (std::find(visited.begin(), end, location) != end)
            return {};
        visited[n] = location;
        auto authorized = result.regions();
        CodeRegions regions { authorized };
        if (!regions.containing(pc)) {
            if (result.count_ == result.views_.size())
                break;
            const auto next = lease.cached_continuation(pc);
            if (!next || !next->inspectable || !next->identity || !next->generation ||
                next->region.begin >= next->region.end || next->region.end > limit ||
                next->region.begin > pc || std::uint64_t { pc } + 4U > next->region.end)
                break;
            result.views_[result.count_++] = { *next, pc };
            result.region_.begin = std::min(result.region_.begin, next->region.begin);
            result.region_.end = std::max(result.region_.end, next->region.end);
            authorized = result.regions();
            regions = CodeRegions { authorized };
            window.regions(regions);
        }
        window.mode(thumb);
        const auto decoded = window.take(pc, it, false);
        if (!decoded || !decoded->word ||
            decoded->instruction.kind == arm::InstructionKind::Unsupported ||
            decoded->instruction.condition == 15)
            break;
        const auto& inst = decoded->instruction;
        const auto next_it = thumb ? (inst.kind == arm::InstructionKind::IfThen
            ? inst.immediate : arm::advance_it(it)) : 0U;
        if (inst.kind == arm::InstructionKind::Branch) {
            pc = ((pc + inst.pc_offset) & (inst.align_pc ? ~3U : ~0U)) + inst.immediate;
            if (inst.exchange) {
                if (next_it != 0 || !source.supports_mixed_instruction_timing())
                    break;
                thumb = !thumb;
            }
        } else {
            if (DynamicBackedge::ends_trace(inst) ||
                (arm::ArchitecturalStop::from(inst) && inst.condition == 14))
                break;
            pc += inst.size;
        }
        it = next_it;
        if (pc == start && thumb == start_thumb && it == start_it)
            break;
    }
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
std::unique_ptr<TraceViews> TraceViews::capture_path(
    InstructionLease& lease, InstructionRegion root, const TracePath& path)
{
    const auto prepared = lease.continuation_region();
    if (!path || !prepared || !same(*prepared, root))
        return {};
    TraceViews result;
    for (unsigned n = 0; n < path.steps(); ++n) {
        const auto pc = path[n].pc;
        const CodeRegions authorized { result.regions() };
        if (authorized.containing(pc))
            continue;
        if (result.count_ == result.views_.size())
            return {};
        const auto view = lease.cached_continuation(pc);
        if (!view || !view->inspectable || !view->identity || !view->generation ||
            view->region.begin >= view->region.end ||
            view->region.end > (std::uint64_t { 1 } << 32U) ||
            view->region.begin > pc || std::uint64_t { pc } + 4U > view->region.end ||
            (n == 0 && !same(view->region, root)))
            return {};
        result.views_[result.count_++] = { *view, pc };
        if (n == 0)
            result.region_ = root;
        else {
            result.region_.begin = std::min(result.region_.begin, view->region.begin);
            result.region_.end = std::max(result.region_.end, view->region.end);
        }
    }
    return std::make_unique<TraceViews>(result);
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
