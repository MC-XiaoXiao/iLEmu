/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "integer_emitter.hpp"
#include "address_cache.hpp"
#include "memory_exits.hpp"
namespace ilemu::execution::arm64 {
class MultipleEmitter {
public:
    MultipleEmitter(oaknut::VectorCodeGenerator& code, bool big_endian,
        AddressCache& addresses)
        : code_(code)
        , big_endian_(big_endian)
        , addresses_(addresses)
    {
    }
    void emit(const arm::Instruction&, std::uint32_t pc, std::uint64_t cost,
        MemoryExits&);

    static std::optional<AddressCache::Key> address(const arm::Instruction&);

private:
    void access(const arm::Instruction&, AddressCache::Reference, MemoryExits&);
    oaknut::VectorCodeGenerator& code_;
    bool big_endian_;
    AddressCache& addresses_;
};
}
