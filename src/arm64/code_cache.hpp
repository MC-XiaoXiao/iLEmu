/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm64/executor.hpp"
#include "compilation_key.hpp"
#include "native_continuations.hpp"
#include "trace_views.hpp"
#include "arm64/shared_compilation_cache.hpp"
#include "arm/architectural_stop.hpp"
#include <list>
#include <map>
#include <tuple>

namespace ilemu::execution::arm64 {
struct NativeOutcome;
// Owns executable storage and opaque view tokens, never guest memory or
// sources.
class CodeCache {
    struct Recency;
public:
    struct Binding {
        std::shared_ptr<const CodeIdentity> identity;
        std::optional<std::uint64_t> generation;
        // Borrowed only until the caller releases its live instruction lease.
        // Query physical provenance lazily on a local miss, keeping hits cheap.
        const InstructionLease* compilation_lease = nullptr;
        InstructionLease* execution_lease = nullptr;
    };
    struct Entry {
        std::shared_ptr<const ExecutableCode> code;
        std::uint64_t maximum_ticks;
        std::optional<arm::ArchitecturalStop> stop;
        bool closed;
        bool accesses_memory;
        bool ends_with_stop;
        std::optional<std::uint32_t> first_instruction;
        std::size_t accounted_bytes;
        CycleTier tier;
        InstructionRegion dependency;
        std::unique_ptr<TraceViews> views;
        std::list<Recency>::iterator recency;
        std::uint64_t continuation_revision = 0;
        unsigned continuation_cooldown = 0;
        unsigned view_heat = CycleTier::maximum_copies;
        bool path_candidate = false, recorded_path = false;
        unsigned path_heat = CycleTier::maximum_copies * CycleTier::maximum_copies;
        bool wants_path() const { return path_candidate && !recorded_path && path_heat == 0; }
        void defer_path()
        {
            path_heat = CycleTier::maximum_copies * CycleTier::maximum_copies *
                CycleTier::maximum_copies;
        }
    };
    // This context borrows the current lease only for the native invocation.
    // Cache-only resolution cannot compile, invalidate, or reclaim live entries.
    class ContinuationScope {
    public:
        ContinuationScope(CodeCache& cache, InstructionLease& lease,
            NativeOutcome& native, MemoryAccess* memory)
            : cache_(cache), lease_(lease), native_(native), memory_(memory) {}
    private:
        friend class CodeCache;
        static NativeContinuation* resolve(
            void*, std::uint32_t pc, std::uint32_t status) noexcept;
        const std::uint64_t* validate_view(const void*, InstructionRegion);
        CodeCache& cache_;
        InstructionLease& lease_;
        NativeOutcome& native_;
        MemoryAccess* memory_;
        NativeContinuations::Table* table_ = nullptr;
    };
    CodeCache(CodeAllocator&, std::size_t, Arm64Statistics&,
        std::shared_ptr<SharedCompilationCache> = {});
    // Recency nodes refer to stable domain storage in this owner.
    CodeCache(CodeCache&&) = delete;
    CodeCache& operator=(CodeCache&&) = delete;
    void clear();
    void invalidate_range(std::uint32_t address, std::size_t size);
    void begin_run();
    void observe(Entry& entry, std::uint64_t ticks)
    {
        observe_visits(entry, 1);
        const bool ready = entry.tier.ready();
        entry.tier.observe(ticks, entry.maximum_ticks);
        if (!ready && entry.tier.ready())
            ++revision_;
    }
    bool prepare_continuations(Entry&, ContinuationScope&, std::uint64_t remaining);
    void finish_continuations(NativeOutcome&);
    Binding bind(InstructionLease&);
    Entry& entry(InstructionSource&, const Binding&, std::uint32_t pc,
        bool step, bool big_endian, bool thumb, unsigned it_state,
        TranslationObserver*,
        std::uint64_t timing_limit = UINT64_MAX,
        std::span<const TraceLocation> path = {});

private:
    void observe_visits(Entry& entry, std::uint64_t visits)
    {
        if (entry.path_candidate && !entry.recorded_path && entry.path_heat != 0) {
            entry.path_heat -= static_cast<unsigned>(
                std::min<std::uint64_t>(entry.path_heat, visits));
            if (entry.path_heat == 0)
                ++revision_;
        }
        if (!entry.closed && !entry.views && entry.view_heat != 0) {
            entry.view_heat -= static_cast<unsigned>(
                std::min<std::uint64_t>(entry.view_heat, visits));
            if (entry.view_heat == 0)
                ++revision_;
        }
    }
    using Key = TranslationKey;
    using Entries = std::map<Key, Entry>;
    struct Domain {
        std::shared_ptr<const CodeIdentity> identity;
        std::optional<std::uint64_t> generation;
        Entries entries;
        std::size_t retained_bytes = 0;
    };
    struct Recency {
        Domain* domain;
        Entries::iterator entry;
    };
    void invalidate(Domain&);
    Entries::iterator erase_entry(Domain&, Entries::iterator);
    void trim(const Binding&, std::size_t accounted);
    Domain* find(const Binding&);
    Domain& obtain(const Binding&);
    NativeContinuation continuation(Entry&);
    void observe_continuation(NativeContinuation&, bool linked);
    static Key continuation_profile(Key);
    std::unique_ptr<NativeContinuations> continuations_;
    std::uint64_t revision_ = 1;
    std::uint64_t continuation_cookie_ = 0;
    CodeAllocator& allocator_;
    std::shared_ptr<SharedCompilationCache> shared_;
    std::size_t budget_;
    Arm64Statistics& stats_;
    Domain anonymous_;
    std::map<const CodeIdentity*, Domain> domains_;
    std::list<Recency> recent_;
    static constexpr std::size_t domain_bytes = sizeof(Domain) + 128U;
    static constexpr std::size_t entry_bytes =
        sizeof(Entry) + sizeof(Key) + 64U + sizeof(Recency) +
        4U * sizeof(void*);
};
}
