/* SPDX-License-Identifier: MPL-2.0 */
#include "code_cache.hpp"
#include "compiler.hpp"
namespace ilemu::execution::arm64 {
const std::uint64_t* CodeCache::ContinuationScope::validate_view(
    const void* domain, InstructionRegion region)
{
    for (auto& view : table_->views) {
        if (view.domain == domain && view.begin == region.begin && view.end == region.end) {
            view.cookie = native_.continuation_cookie;
            return &view.cookie;
        }
    }
    for (auto& view : table_->views) {
        if (view.cookie == native_.continuation_cookie)
            continue;
        // Never reuse a live validation cell. Discard old descriptors before
        // changing an inactive cell's region, so old hits cannot borrow it.
        for (auto& descriptor : table_->entries) {
            if (descriptor.validation != &view.cookie)
                continue;
            cache_.observe_continuation(descriptor, true);
            descriptor = {};
        }
        view = { domain, region.begin, region.end, native_.continuation_cookie };
        return &view.cookie;
    }
    return nullptr;
}
NativeContinuation* CodeCache::ContinuationScope::resolve(
    void* context, std::uint32_t pc, std::uint32_t status) noexcept
{
    auto& scope = *static_cast<ContinuationScope*>(context);
    auto& cache = scope.cache_;
    ++cache.stats_.continuation_resolutions;
    try {
        if ((pc & ((status & 0x20U) ? 1U : 3U)) != 0)
            return nullptr;
        const auto view = scope.lease_.cached_continuation(pc);
        if (!view || !view->identity || !view->generation ||
            pc < view->region.begin || std::uint64_t { pc } + 4 > view->region.end)
            return nullptr;
        const Binding binding { view->identity, view->generation, nullptr };
        auto* domain = cache.find(binding);
        if (!domain || domain->generation != view->generation)
            return nullptr;
        auto key = scope.table_->profile;
        std::get<0>(key) = pc;
        std::get<2>(key) = (status & 0x200U) != 0;
        std::get<3>(key) = (status & 0x20U) != 0;
        std::get<4>(key) = ((status >> 25U) & 3U) | ((status >> 8U) & 0xfcU);
        const auto found = domain->entries.find(key);
        if (found == domain->entries.end())
            return nullptr;
        auto& entry = found->second;
        if (!entry.code || entry.stop || entry.maximum_ticks == 0 || entry.tier.ready())
            return nullptr;
        // A warmed path must reach the ordinary owner before it can acquire
        // dependencies or replace code. Native resolution remains cache-only.
        if ((entry.wants_path() ||
                (!entry.closed && !entry.views && entry.view_heat == 0)) && view->inspectable)
            return nullptr;
        if (entry.views) {
            if (!entry.views->pin(scope.lease_, view->region,
                    TraceViews::EntryAuthorization::Cached))
                return nullptr;
        } else if (entry.dependency.begin < view->region.begin ||
                   entry.dependency.end > view->region.end)
            return nullptr;
        // Pinning an executable shared alias may revoke unsafe direct stores.
        // Each tail entry reloads these tables; no invocation-local page cache
        // remains live after the previous trace's canonical-state epilogue.
        if (scope.memory_)
            scope.native_.memory = scope.memory_->direct_memory();
        // Multi-view entries own their complete dependency cookie. Only entries
        // confined to one view share the table's regional validation cells.
        const auto* validation = entry.views
            ? entry.views->continuation_validation(scope.native_.continuation_cookie)
            : scope.validate_view(domain, view->region);
        if (!validation)
            return nullptr;
        auto& slot = scope.table_->select(pc, status);
        // A collision may replace a fragment already visited in this scope.
        // Account its separate invocations before replacing the borrowed slot.
        cache.observe_continuation(slot, true);
        slot = cache.continuation(entry);
        slot.validation = validation;
        ++cache.stats_.continuation_resolution_hits;
        return &slot;
    } catch (...) {
        // This acceleration never throws across generated code. A normal
        // dispatcher iteration performs the authoritative fetch or fault.
        return nullptr;
    }
}
}
