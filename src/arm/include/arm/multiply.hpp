/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
namespace ilemu::arm {
// Multiply uses rd for the low result and rn for the high result or word
// addend. rm/rs are the inputs, accumulate selects MLA or long accumulation,
// and sign_extend selects a signed product. Halfword shift_amount bits 0/1
// select the upper half of rm/rs respectively. High-word operations reuse
// shift_amount for the rounding bit. No extra state is stored.
enum class MultiplyOperation { Word, Subtract, Long, DualAccumulateLong,
    High, HighSubtract, Halfword };
constexpr bool multiply_writes_pair(const Instruction& inst)
{
    return inst.opcode == static_cast<unsigned>(MultiplyOperation::Long) ||
           inst.opcode == static_cast<unsigned>(MultiplyOperation::DualAccumulateLong);
}
std::uint64_t multiply(const Instruction&, std::uint32_t first,
    std::uint32_t second, std::uint32_t low, std::uint32_t high_or_addend) noexcept;
std::uint32_t multiply_nz(const Instruction&, std::uint32_t cpsr,
    std::uint64_t result) noexcept;
}
