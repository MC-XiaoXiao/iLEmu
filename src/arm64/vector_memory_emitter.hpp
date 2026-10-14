/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "address_cache.hpp"
#include "arm/instruction.hpp"
#include "vector_registers.hpp"
#include <oaknut/oaknut.hpp>

namespace ilemu::execution::arm64 {
class VectorMemoryEmitter {
public:
    VectorMemoryEmitter(oaknut::VectorCodeGenerator& code, bool big_endian,
        AddressCache& addresses, VectorRegisters& vectors)
        : code_(code)
        , big_endian_(big_endian)
        , addresses_(addresses)
        , vectors_(vectors)
    {
    }
    void emit(const arm::Instruction&, std::uint32_t pc, std::uint64_t cost,
        oaknut::Label& checked_exit);

private:
    template<typename Element>
    void access_lane(bool load, Element element, oaknut::XReg pointer)
    {
        if (load)
            code_.LD1(oaknut::List { element }, pointer);
        else
            code_.ST1(oaknut::List { element }, pointer);
    }
    void access(const arm::Instruction&, oaknut::XReg pointer);
    void scalar_multiple(const arm::Instruction&, oaknut::XReg pointer);
    void endian(oaknut::QReg, unsigned size, bool quad);
    oaknut::VectorCodeGenerator& code_;
    bool big_endian_;
    AddressCache& addresses_;
    VectorRegisters& vectors_;
};
}
