/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "address_cache.hpp"
#include "integer_emitter.hpp"
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
        bool big_endian)
        : code_(code)
        , integer_(integer)
        , big_endian_(big_endian)
        , addresses_(code)
    {
    }
    void emit(const arm::Instruction&, std::uint32_t pc, std::uint64_t cost,
        Exits&);
    void invalidate_register(unsigned reg) { addresses_.invalidate(reg); }
    void invalidate_all() { addresses_.clear(); }
    AddressCache& addresses() { return addresses_; }

private:
    std::optional<AddressCache::Key> address(
        const arm::Instruction&, std::uint32_t pc) const;
    void access(const arm::Instruction&, std::uint32_t pc, oaknut::XReg,
        Exits&);
    void store_value(const arm::Instruction&, std::uint32_t pc);
    void endian(unsigned size, oaknut::WReg value);
    oaknut::VectorCodeGenerator& code_;
    IntegerEmitter& integer_;
    bool big_endian_;
    AddressCache addresses_;
};
}
