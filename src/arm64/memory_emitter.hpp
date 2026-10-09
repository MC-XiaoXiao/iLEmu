/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "integer_emitter.hpp"
#include <array>
#include <optional>

namespace ilemu::execution::arm64 {
class MemoryEmitter {
public:
    MemoryEmitter(oaknut::VectorCodeGenerator& code, IntegerEmitter& integer,
        bool big_endian)
        : code_(code)
        , integer_(integer)
        , big_endian_(big_endian)
    {
    }
    void emit(const arm::Instruction&, std::uint32_t pc, std::uint64_t cost,
        oaknut::Label& checked_exit, oaknut::Label& unsupported_exit,
        oaknut::Label& branch_exit);
    void invalidate_register(unsigned reg);
    void invalidate_all() { addresses_ = { }; }

private:
    struct Address {
        unsigned base, size;
        std::uint32_t offset;
        bool add, load;
        bool operator==(const Address&) const = default;
    };
    std::optional<Address> address(const arm::Instruction&) const;
    void access(const arm::Instruction&, std::uint32_t pc, oaknut::XReg,
        oaknut::Label& unsupported_exit, oaknut::Label& branch_exit);
    void store_value(const arm::Instruction&, std::uint32_t pc);
    void endian(unsigned size, oaknut::WReg value);
    oaknut::VectorCodeGenerator& code_;
    IntegerEmitter& integer_;
    bool big_endian_;
    // Entries describe dominating unconditional guards in this emitted trace.
    // Their registers are initialized again on every trace-head traversal.
    std::array<std::optional<Address>, 2> addresses_;
    unsigned next_address_ = 0;
};
}
