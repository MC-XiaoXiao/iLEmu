/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "shift_count_range.hpp"
#include <oaknut/oaknut.hpp>
#include <array>
#include <optional>
namespace ilemu::execution::arm64 {
// Carry-only lowering may clobber N/Z/V. The trace must prove that all three
// are overwritten before any read, exit or other observable boundary.
enum class LogicalFlagOutput { All, CarryOnly };
// Integer instruction emission is independent of trace exits and scheduling.
class IntegerEmitter {
public:
    explicit IntegerEmitter(oaknut::VectorCodeGenerator& code,
        std::optional<RegisterShiftRange> shift_range = {})
        : code_(code), shift_range_(shift_range)
    {
    }
    void enter_shift_range();
    // pc_value is the encoding's architectural PC operand, including alignment.
    oaknut::WReg reg(
        unsigned index, std::uint32_t pc_value, oaknut::WReg scratch);
    void alu(const arm::Instruction&, std::uint32_t pc,
        LogicalFlagOutput = LogicalFlagOutput::All);
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
    void parallel_arithmetic(const arm::Instruction&);
    void saturating_arithmetic(const arm::Instruction&);
    // Scratch SIMD values are reusable only along unconditional adjacent ops.
    void clear_parallel_values() { parallel_values_.fill(std::nullopt); }
    void clear_multiply_addend() { multiply_addend_.reset(); }
    void invalidate_multiply_addend(unsigned reg)
    {
        if (multiply_addend_ && multiply_addend_->reg == reg)
            clear_multiply_addend();
    }

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
    bool dsp_multiply(const arm::Instruction&);
    bool dual_halfword_multiply(const arm::Instruction&);
    void merge_nz();
    bool logical_immediate(const arm::Instruction&, std::uint32_t pc, LogicalFlagOutput);
    oaknut::WReg lhs(const arm::Instruction&, std::uint32_t pc);
    void carry_from(oaknut::WReg value, unsigned bit,
        LogicalFlagOutput = LogicalFlagOutput::All);
    void shifter(const arm::Instruction&, std::uint32_t pc, bool carry_output,
        LogicalFlagOutput = LogicalFlagOutput::All);
    bool shifted_alu(const arm::Instruction&, std::uint32_t pc);
    void bounded_shift(const arm::Instruction&, oaknut::WReg destination,
        oaknut::WReg source);
    void bounded_shifter(const arm::Instruction&, oaknut::WReg destination,
        oaknut::WReg source, bool carry_output, LogicalFlagOutput);
    oaknut::VectorCodeGenerator& code_;
    std::optional<RegisterShiftRange> shift_range_;
    // Compile-time knowledge only. Each operation still writes its GPR result.
    std::array<std::optional<ParallelValue>, 3> parallel_values_ {};
    std::optional<MultiplyAddend> multiply_addend_;
};
}
