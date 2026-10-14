/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "address_cache.hpp"
#include "memory_exits.hpp"
#include "integer_emitter.hpp"
#include "read_page_cache.hpp"
#include <list>
#include <memory_resource>
#include <optional>

namespace ilemu::execution::arm64 {
class MemoryEmitter {
public:
    using ExitKind = MemoryExits::Kind;
    using Exits = MemoryExits;
    MemoryEmitter(oaknut::VectorCodeGenerator& code, IntegerEmitter& integer,
        bool big_endian, bool expanded, bool cache_reads,
        std::pmr::memory_resource* temporary,
        const AddressWindows* windows = nullptr)
        : code_(code)
        , integer_(integer)
        , big_endian_(big_endian)
        // Only the existing hot tier earns extra ABI save/restore state.
        , addresses_(code, expanded && !cache_reads, windows)
        , read_pages_(code, cache_reads)
        , fast_reads_(expanded)
        , pending_(temporary)
    {
    }
    void emit(const arm::Instruction&, std::uint32_t pc, std::uint64_t cost,
        Exits&);
    void finish();
    void enter(oaknut::VectorCodeGenerator& code) const
    {
        read_pages_.enter(code);
        addresses_.enter(code);
    }
    void leave() const
    {
        addresses_.leave();
        read_pages_.leave();
    }
    unsigned read_expansion_growth() const { return read_pages_.extra_words(); }
    static std::optional<AddressCache::Key> address(
        const arm::Instruction&, std::uint32_t pc);
    static std::optional<unsigned> dependent_read_destination(
        const arm::Instruction&, std::uint32_t pc);
    void invalidate_register(unsigned reg) { addresses_.invalidate(reg); }
    void invalidate_all() { addresses_.clear(); }
    AddressCache& addresses() { return addresses_; }

private:
    static bool is_dependent_read(
        const arm::Instruction&, const AddressCache::Key&);
    void access(const arm::Instruction&, std::uint32_t pc, AddressCache::Reference,
        Exits&);
    void dependent_read(const arm::Instruction&, std::uint32_t pc,
        std::uint64_t cost, Exits&);
    struct ReadExit {
        arm::Instruction instruction;
        std::uint64_t cost;
        oaknut::WReg address;
        oaknut::Label entry;
        oaknut::Label* checked;
    };
    void store_value(const arm::Instruction&, std::uint32_t pc);
    void endian(unsigned size, oaknut::WReg value);
    std::pmr::memory_resource& label_memory();
    oaknut::VectorCodeGenerator& code_;
    IntegerEmitter& integer_;
    bool big_endian_;
    AddressCache addresses_;
    ReadPageCache read_pages_;
    bool fast_reads_, read_table_guarded_ = false;
    // Reuse short-lived guard fixups within this compile. Laziness keeps
    // traces without scalar guards free of pool setup and allocation.
    std::optional<std::pmr::unsynchronized_pool_resource> labels_;
    std::pmr::list<ReadExit> pending_;
};
}
