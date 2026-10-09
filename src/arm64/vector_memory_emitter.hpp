/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "address_cache.hpp"
#include "arm/instruction.hpp"
#include <oaknut/oaknut.hpp>

namespace ilemu::execution::arm64 {
class VectorMemoryEmitter {
public:
    VectorMemoryEmitter(oaknut::VectorCodeGenerator& code, bool big_endian,
        AddressCache& addresses)
        : code_(code)
        , big_endian_(big_endian)
        , addresses_(addresses)
    {
    }
    void emit(const arm::Instruction&, std::uint64_t cost,
        oaknut::Label& checked_exit);

private:
    void access(const arm::Instruction&, oaknut::XReg pointer);
    oaknut::VectorCodeGenerator& code_;
    bool big_endian_;
    AddressCache& addresses_;
};
}
