/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "address_cache.hpp"
#include "integer_emitter.hpp"
#include <list>
#include <memory_resource>
#include <optional>

namespace ilemu::execution::arm64 {
class MemoryEmitter {
public:
    enum class ExitKind { Checked, Unsupported, Branch };
    // Resolve exits only when an emitted path references them. A cached data
    // access can return without creating any new exit payloads.
    class Exits {
    public:
        virtual ~Exits() = default;
        virtual oaknut::Label& get(ExitKind) = 0;
    };
    MemoryEmitter(oaknut::VectorCodeGenerator& code, IntegerEmitter& integer,
        bool big_endian, bool fast_reads,
        std::pmr::memory_resource* temporary)
        : code_(code)
        , integer_(integer)
        , big_endian_(big_endian)
        , addresses_(code)
        , fast_reads_(fast_reads)
        , pending_(temporary)
    {
    }
    void emit(const arm::Instruction&, std::uint32_t pc, std::uint64_t cost,
        Exits&);
    void finish();
    void invalidate_register(unsigned reg) { addresses_.invalidate(reg); }
    void invalidate_all() { addresses_.clear(); }
    AddressCache& addresses() { return addresses_; }

private:
    std::optional<AddressCache::Key> address(
        const arm::Instruction&, std::uint32_t pc) const;
    void access(const arm::Instruction&, std::uint32_t pc, oaknut::XReg,
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
    oaknut::VectorCodeGenerator& code_;
    IntegerEmitter& integer_;
    bool big_endian_;
    AddressCache addresses_;
    bool fast_reads_, read_table_guarded_ = false;
    std::pmr::list<ReadExit> pending_;
};
}
