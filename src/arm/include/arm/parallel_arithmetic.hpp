/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/integer_semantics.hpp"
namespace ilemu::arm {
enum class ParallelArithmeticOperation {
    Add16, AddSubtract, SubtractAdd, Subtract16, Add8, Subtract8, Select
};
struct ParallelArithmeticLayout {
    unsigned bits;
    bool exchange, subtract_low, subtract_high;
    bool subtract(unsigned offset) const noexcept
    { return offset < 16 ? subtract_low : subtract_high; }
};
constexpr ParallelArithmeticLayout parallel_arithmetic_layout(
    ParallelArithmeticOperation operation) noexcept
{
    using Op = ParallelArithmeticOperation;
    switch (operation) {
    case Op::Add16: return {16, false, false, false};
    case Op::AddSubtract: return {16, true, true, false};
    case Op::SubtractAdd: return {16, true, false, true};
    case Op::Subtract16: return {16, false, true, true};
    case Op::Add8: return {8, false, false, false};
    case Op::Subtract8: return {8, false, true, true};
    case Op::Select: return {8, false, false, false};
    }
    return {8, false, false, false};
}
AluResult parallel_arithmetic(const Instruction&, std::uint32_t first,
    std::uint32_t second, std::uint32_t cpsr) noexcept;
}
