/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm64/executor.hpp"
#include "cycle_tier.hpp"
#include "arm/architectural_stop.hpp"
#include <map>
#include <tuple>

namespace ilemu::execution::arm64 {
// Owns executable storage and opaque view tokens, never guest memory or
// sources.
class CodeCache {
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
    };
    CodeCache(CodeAllocator&, std::size_t, Arm64Statistics&);
    void clear();
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
    struct Domain {
        std::shared_ptr<const CodeIdentity> identity;
        std::optional<std::uint64_t> generation;
        std::map<Key, Entry> entries;
        std::size_t retained_bytes = 0;
    };
    void invalidate(Domain&);
    Domain* find(const Binding&);
    Domain& obtain(const Binding&);
    CodeAllocator& allocator_;
    std::size_t budget_;
    Arm64Statistics& stats_;
    Domain anonymous_;
    std::map<const CodeIdentity*, Domain> domains_;
    static constexpr std::size_t domain_bytes = sizeof(Domain) + 128U;
    static constexpr std::size_t entry_bytes =
        sizeof(Entry) + sizeof(Key) + 64U;
};
}
