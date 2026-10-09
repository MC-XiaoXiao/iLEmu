/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include <oaknut/oaknut.hpp>
namespace ilemu::execution::arm64 {
// Integer instruction emission is independent of trace exits and scheduling.
class IntegerEmitter {
public:
    explicit IntegerEmitter(oaknut::VectorCodeGenerator& code)
        : code_(code)
    {
    }
    oaknut::WReg reg(unsigned index, std::uint32_t pc, oaknut::WReg scratch);
    void alu(const arm::Instruction&, std::uint32_t pc);
    void address_offset(const arm::Instruction& inst, std::uint32_t pc)
    {
        shifter(inst, pc, false);
    }
    void multiply(const arm::Instruction&);

private:
    void merge_nz();
    void carry_from(oaknut::WReg value, unsigned bit);
    void shifter(
        const arm::Instruction&, std::uint32_t pc, bool carry_output);
    bool shifted_alu(const arm::Instruction&, std::uint32_t pc);
    oaknut::VectorCodeGenerator& code_;
};
}
