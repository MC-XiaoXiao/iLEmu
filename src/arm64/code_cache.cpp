/* SPDX-License-Identifier: MPL-2.0 */
#include "code_cache.hpp"
#include "compiler.hpp"
#include "published_trace.hpp"
#include <algorithm>
#include <span>
#include <stdexcept>

namespace ilemu::execution::arm64 {
CodeCache::CodeCache(
    CodeAllocator& allocator, std::size_t budget, Arm64Statistics& stats,
    std::shared_ptr<SharedCompilationCache> shared)
    : allocator_(allocator)
    , shared_(std::move(shared))
    , budget_(budget)
    , stats_(stats)
{
    if (shared_ && !shared_->owns(allocator))
        throw std::invalid_argument("shared code cache allocator mismatch");
    if (budget < Arm64Executor::minimum_cache_bytes)
        throw std::invalid_argument(
            "ARM64 code cache requires at least 256 KiB");
}
void CodeCache::invalidate(Domain& domain)
{
    for (const auto& [key, entry] : domain.entries)
        recent_.erase(entry.recency);
    domain.entries.clear();
    stats_.retained_bytes -= domain.retained_bytes;
    domain.retained_bytes = 0;
    domain.generation.reset();
}
void CodeCache::clear()
{
    recent_.clear();
    anonymous_ = { };
    domains_.clear();
    stats_.retained_bytes = 0;
}
void CodeCache::begin_run()
{
    invalidate(anonymous_);
}
void CodeCache::invalidate_range(std::uint32_t address, std::size_t size)
{
    if (size == 0)
        return;
    constexpr auto address_limit = std::uint64_t { 1 } << 32U;
    const auto begin = std::uint64_t { address };
    const auto end = begin + std::min<std::uint64_t>(size, address_limit - begin);
    const auto overlaps = [=](std::uint64_t first, std::uint64_t last) {
        return begin < last && first < end;
    };
    const auto invalidate_entries = [&](Domain& domain) {
        for (auto it = domain.entries.begin(); it != domain.entries.end();) {
            const auto& dependency = it->second.dependency;
            const auto pc = std::uint64_t { std::get<0>(it->first) };
            // The entry fetch may straddle the source region, including the
            // second halfword of a Thumb instruction at the address-space end.
            const auto fetch_end = pc + 4U;
            if (overlaps(dependency.begin, dependency.end) ||
                overlaps(pc, std::min(fetch_end, address_limit)) ||
                (fetch_end > address_limit &&
                    overlaps(0, fetch_end - address_limit))) {
                it = erase_entry(domain, it);
            } else {
                ++it;
            }
        }
    };
    invalidate_entries(anonymous_);
    for (auto it = domains_.begin(); it != domains_.end();) {
        invalidate_entries(it->second);
        if (it->second.entries.empty()) {
            stats_.retained_bytes -= domain_bytes;
            it = domains_.erase(it);
        } else {
            ++it;
        }
    }
}
CodeCache::Entries::iterator CodeCache::erase_entry(
    Domain& domain, Entries::iterator entry)
{
    domain.retained_bytes -= entry->second.accounted_bytes;
    stats_.retained_bytes -= entry->second.accounted_bytes;
    recent_.erase(entry->second.recency);
    return domain.entries.erase(entry);
}
void CodeCache::trim(const Binding& binding, std::size_t accounted)
{
    const auto fits = [&] {
        const auto needed = accounted +
            (binding.identity && !find(binding) ? domain_bytes : 0);
        return stats_.retained_bytes <= budget_ - needed;
    };
    if (fits())
        return;
    // Epoch invalidation may leave empty domains. Reclaim their metadata
    // before evicting code, including the domain currently being refilled.
    for (auto it = domains_.begin(); it != domains_.end();) {
        if (it->second.entries.empty()) {
            stats_.retained_bytes -= domain_bytes;
            it = domains_.erase(it);
        } else {
            ++it;
        }
    }
    while (!fits()) {
        if (recent_.empty())
            throw std::logic_error("inconsistent ARM64 cache accounting");
        const auto oldest = recent_.front();
        auto& domain = *oldest.domain;
        erase_entry(domain, oldest.entry);
        if (domain.entries.empty() && domain.identity) {
            domains_.erase(domain.identity.get());
            stats_.retained_bytes -= domain_bytes;
        }
    }
}
CodeCache::Domain* CodeCache::find(const Binding& binding)
{
    if (!binding.identity)
        return &anonymous_;
    const auto it = domains_.find(binding.identity.get());
    return it == domains_.end() ? nullptr : &it->second;
}
CodeCache::Domain& CodeCache::obtain(const Binding& binding)
{
    if (auto* domain = find(binding)) {
        domain->generation = binding.generation;
        return *domain;
    }
    auto [it, inserted] = domains_.try_emplace(binding.identity.get());
    if (inserted) {
        it->second.identity = binding.identity;
        stats_.retained_bytes += domain_bytes;
    }
    it->second.generation = binding.generation;
    return it->second;
}
CodeCache::Binding CodeCache::bind(const InstructionLease& lease)
{
    Binding binding { lease.identity(), lease.generation(),
        shared_ ? &lease : nullptr };
    if (!binding.generation) {
        // Losing the epoch revokes any previously persistent translations.
        if (binding.identity) {
            if (const auto it = domains_.find(binding.identity.get());
                it != domains_.end()) {
                invalidate(it->second);
                domains_.erase(it);
                stats_.retained_bytes -= domain_bytes;
            }
        }
        binding.identity.reset();
        binding.compilation_lease = nullptr;
        invalidate(anonymous_);
    } else if (auto* domain = find(binding);
        domain && domain->generation != binding.generation) {
        invalidate(*domain);
    }
    return binding;
}
CodeCache::Entry& CodeCache::entry(InstructionSource& source,
    const Binding& binding, std::uint32_t pc, bool step, bool big_endian,
    bool thumb, unsigned it_state, TranslationObserver* observer,
    std::uint64_t timing_limit)
{
    const Key key { pc, step, big_endian, thumb, it_state,
        source.data_memory() != nullptr, timing_limit,
        source.thumb_arithmetic_profile(), source.vfp_register_profile(),
        source.arm_instruction_profile(), source.floating_exception_profile(),
        source.alu_pc_write_profile(), source.vfp_instruction_profile(),
        source.integer_divide_profile() };
    auto expansion = binding.generation ? CycleExpansion::Compact
                                       : CycleExpansion::Untracked;
    if (auto* domain = find(binding)) {
        if (const auto it = domain->entries.find(key);
            it != domain->entries.end()) {
            if (!it->second.tier.ready()) {
                recent_.splice(recent_.end(), recent_, it->second.recency);
                ++stats_.cache_hits;
                return it->second;
            }
            expansion = CycleExpansion::Hot;
        }
    }
    const auto started = std::chrono::steady_clock::now();
    const auto region = source.instruction_region(pc);
    std::optional<CompilationKey> shared_key;
    if (binding.compilation_lease) {
        const auto provenance = binding.compilation_lease->compilation_identity();
        if (provenance && provenance->semantics && provenance->page_count != 0 &&
            provenance->page_count <= provenance->pages.size() &&
            provenance->pages[0].storage != 0 &&
            (provenance->page_count != 2 || provenance->pages[1].storage != 0))
            shared_key = CompilationKey { *provenance, key, expansion,
                region.begin, region.end };
    }
    auto plan = shared_key ? shared_->find(*shared_key) : nullptr;
    const bool reused = bool(plan);
    if (!plan) {
        const auto compiled = compile(source, pc, step, timing_limit,
            big_endian, thumb, it_state, expansion);
        const auto bytes = std::as_bytes(std::span { compiled.words });
        if (bytes.size() > 256U * 1024U)
            throw std::length_error("ARM64 trace exceeds code limit");
        std::unique_ptr<ExecutableCode> allocation;
        if (!compiled.stop) {
            allocation = allocator_.allocate(bytes.size());
            if (!allocation)
                throw std::bad_alloc { };
        }
        const auto capacity = allocation ? allocation->capacity() : 0;
        // Reserve domain overhead even if eviction will recreate this domain.
        const auto overhead = entry_bytes + (binding.identity ? domain_bytes : 0);
        if (capacity > budget_ - overhead)
            throw std::length_error("ARM64 trace exceeds cache budget");
        if (allocation) {
            allocation->publish(bytes);
            if (!allocation->entry())
                throw std::runtime_error("ARM64 code was not published");
        }
        plan = std::make_shared<const PublishedTrace>(PublishedTrace {
            static_cast<const TraceMetadata&>(compiled),
            std::move(allocation), bytes.size() });
    }
    const auto overhead = entry_bytes + (binding.identity ? domain_bytes : 0);
    if (plan->capacity() > budget_ - overhead)
        throw std::length_error("ARM64 trace exceeds cache budget");
    const auto accounted = plan->capacity() + entry_bytes;
    // Entry lookup happens between native invocations under a fresh code
    // lease. Publish successfully before replacing the cold allocation.
    if (auto* domain = find(binding)) {
        if (auto previous = domain->entries.find(key);
            previous != domain->entries.end()) {
            erase_entry(*domain, previous);
        }
    }
    // Only called between native invocations; no active entry is reclaimed.
    trim(binding, accounted);
    auto& domain = obtain(binding);
    auto [it, inserted] = domain.entries.emplace(key,
        Entry { plan->code, plan->maximum_ticks, plan->stop,
            plan->closed, plan->accesses_memory, plan->ends_with_stop,
            plan->first_instruction, accounted,
            CycleTier { plan->closed, expansion == CycleExpansion::Hot },
            source.instruction_region(pc), { } });
    if (!inserted)
        throw std::logic_error("duplicate ARM64 cache entry");
    try {
        it->second.recency = recent_.insert(recent_.end(), { &domain, it });
    } catch (...) {
        domain.entries.erase(it);
        throw;
    }
    domain.retained_bytes += accounted;
    stats_.retained_bytes += accounted;
    // Publish and install successfully before making the plan reusable. Cache
    // metadata allocation failure is optional and cannot fail guest execution.
    if (shared_key && !reused) {
        try { shared_->insert(*shared_key, plan); }
        catch (const std::bad_alloc&) { }
    }
    if (reused)
        ++stats_.shared_compilation_hits;
    // Shared hits report their actual lookup and local installation cost.
    ++stats_.compiled_regions;
    stats_.emitted_bytes += plan->emitted_bytes;
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    stats_.compilation_nanoseconds += elapsed;
    if (observer)
        observer->translation_completed(elapsed);
    return it->second;
}
}
