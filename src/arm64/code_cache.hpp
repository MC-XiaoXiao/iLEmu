/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm64/executor.hpp"
#include "cycle_tier.hpp"
#include "arm/architectural_stop.hpp"
#include <list>
#include <map>
#include <tuple>

namespace ilemu::execution::arm64 {
// Owns executable storage and opaque view tokens, never guest memory or
// sources.
class CodeCache {
    struct Recency;
public:
    struct Binding {
        std::shared_ptr<const CodeIdentity> identity;
        std::optional<std::uint64_t> generation;
    };
    struct Entry {
        std::unique_ptr<ExecutableCode> code;
        std::uint64_t maximum_ticks;
        std::optional<arm::ArchitecturalStop> stop;
        bool closed;
        bool accesses_memory;
        bool ends_with_stop;
        std::optional<std::uint32_t> first_instruction;
        std::size_t accounted_bytes;
        CycleTier tier;
        InstructionRegion dependency;
        std::list<Recency>::iterator recency;
    };
    CodeCache(CodeAllocator&, std::size_t, Arm64Statistics&);
    // Recency nodes refer to stable domain storage in this owner.
    CodeCache(CodeCache&&) = delete;
    CodeCache& operator=(CodeCache&&) = delete;
    void clear();
    void invalidate_range(std::uint32_t address, std::size_t size);
    void begin_run();
    void observe(Entry& entry, std::uint64_t ticks)
    {
        entry.tier.observe(ticks, entry.maximum_ticks);
    }
    Binding bind(const InstructionLease&);
    Entry& entry(InstructionSource&, const Binding&, std::uint32_t pc,
        bool step, bool big_endian, bool thumb, unsigned it_state,
        TranslationObserver*,
        std::uint64_t timing_limit = UINT64_MAX);

private:
    using Key = std::tuple<std::uint32_t, bool, bool, bool, unsigned, bool,
        std::uint64_t, ThumbArithmeticProfile, VfpRegisterProfile,
        ArmInstructionProfile, FloatingExceptionProfile, AluPcWriteProfile,
        VfpInstructionProfile, IntegerDivideProfile>;
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
    CodeAllocator& allocator_;
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
