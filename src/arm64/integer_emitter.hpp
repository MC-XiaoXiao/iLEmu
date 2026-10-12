/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include <oaknut/oaknut.hpp>
#include <array>
#include <optional>
namespace ilemu::execution::arm64 {
// Integer instruction emission is independent of trace exits and scheduling.
class IntegerEmitter {
public:
    explicit IntegerEmitter(oaknut::VectorCodeGenerator& code)
        : code_(code)
    {
    }
    // pc_value is the encoding's architectural PC operand, including alignment.
    oaknut::WReg reg(
        unsigned index, std::uint32_t pc_value, oaknut::WReg scratch);
    void alu(const arm::Instruction&, std::uint32_t pc);
    void address_offset(const arm::Instruction& inst, std::uint32_t pc)
    {
        shifter(
            inst, (pc + inst.pc_offset) & (inst.align_pc ? ~3U : ~0U), false);
    }
    void multiply(const arm::Instruction&);
    void divide(const arm::Instruction&);
    void wide_immediate(const arm::Instruction&);
    void pack_halfword(const arm::Instruction&);
    void transform(const arm::Instruction&);
    void parallel_saturating(const arm::Instruction&);
    // Scratch SIMD values are reusable only along unconditional adjacent ops.
    void clear_parallel_values() { parallel_values_.fill(std::nullopt); }
    void clear_multiply_addend() { multiply_addend_.reset(); }

private:
    struct ParallelValue {
        unsigned reg;
        bool exchange;
    };
    struct MultiplyAddend {
        unsigned reg;
        bool round;
    };
    unsigned parallel_source(unsigned reg, bool exchange, unsigned protected_mask);
    void merge_nz();
    oaknut::WReg lhs(const arm::Instruction&, std::uint32_t pc);
    void carry_from(oaknut::WReg value, unsigned bit);
    void shifter(const arm::Instruction&, std::uint32_t pc, bool carry_output);
    bool shifted_alu(const arm::Instruction&, std::uint32_t pc);
    oaknut::VectorCodeGenerator& code_;
    // Compile-time knowledge only. Each operation still writes its GPR result.
    std::array<std::optional<ParallelValue>, 3> parallel_values_ {};
    std::optional<MultiplyAddend> multiply_addend_;
};
}
