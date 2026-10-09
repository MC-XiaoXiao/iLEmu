/* SPDX-License-Identifier: MPL-2.0 */
#include "code_cache.hpp"
#include "compiler.hpp"
#include <span>
#include <stdexcept>

namespace ilemu::execution::arm64 {
CodeCache::CodeCache(
    CodeAllocator& allocator, std::size_t budget, Arm64Statistics& stats)
    : allocator_(allocator)
    , budget_(budget)
    , stats_(stats)
{
    if (budget < 256U * 1024U)
        throw std::invalid_argument(
            "ARM64 code cache requires at least 256 KiB");
}
void CodeCache::invalidate(Domain& domain)
{
    domain.entries.clear();
    stats_.retained_bytes -= domain.retained_bytes;
    domain.retained_bytes = 0;
    domain.generation.reset();
}
void CodeCache::clear()
{
    anonymous_ = { };
    domains_.clear();
    stats_.retained_bytes = 0;
}
void CodeCache::begin_run() { invalidate(anonymous_); }
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
    Binding binding { lease.identity(), lease.generation() };
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
        invalidate(anonymous_);
    } else if (auto* domain = find(binding);
        domain && domain->generation != binding.generation) {
        invalidate(*domain);
    }
    return binding;
}
CodeCache::Entry& CodeCache::entry(InstructionSource& source,
    const Binding& binding, std::uint32_t pc, bool step, bool big_endian,
    bool thumb, unsigned it_state, std::uint64_t timing_limit)
{
    const Key key { pc, step, big_endian, thumb, it_state,
        source.data_memory() != nullptr, timing_limit };
    if (auto* domain = find(binding)) {
        if (const auto it = domain->entries.find(key);
            it != domain->entries.end()) {
            ++stats_.cache_hits;
            return it->second;
        }
    }
    const auto started = std::chrono::steady_clock::now();
    const auto compiled =
        compile(source, pc, step, timing_limit, big_endian, thumb, it_state);
    const auto bytes = std::as_bytes(std::span { compiled.words });
    if (bytes.size() > 256U * 1024U)
        throw std::length_error("ARM64 trace exceeds code limit");
    auto allocation = allocator_.allocate(bytes.size());
    if (!allocation)
        throw std::bad_alloc { };
    const auto capacity = allocation->capacity();
    // Reserve domain overhead even if eviction will recreate this domain.
    const auto overhead = entry_bytes + (binding.identity ? domain_bytes : 0);
    if (capacity > budget_ - overhead)
        throw std::length_error("ARM64 trace exceeds cache budget");
    const auto accounted = capacity + entry_bytes;
    const auto needed =
        accounted + (binding.identity && !find(binding) ? domain_bytes : 0);
    // Only called between native invocations; no active entry is reclaimed.
    if (stats_.retained_bytes > budget_ - needed)
        clear();
    allocation->publish(bytes);
    if (!allocation->entry())
        throw std::runtime_error("ARM64 code was not published");
    auto& domain = obtain(binding);
    auto [it, inserted] = domain.entries.emplace(key,
        Entry { std::move(allocation), compiled.maximum_ticks, compiled.closed,
            compiled.accesses_memory, compiled.first_instruction, accounted });
    if (!inserted)
        throw std::logic_error("duplicate ARM64 cache entry");
    domain.retained_bytes += accounted;
    stats_.retained_bytes += accounted;
    ++stats_.compiled_regions;
    stats_.emitted_bytes += bytes.size();
    stats_.compilation_nanoseconds += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    return it->second;
}
}
