/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/parallel_arithmetic.hpp"
namespace ilemu::arm {
// ParallelSaturating uses opcode for the operation and rd/rn/rm for GPRs.
enum class ParallelSaturatingOperation {
    UnsignedAdd16,
    UnsignedAddSubtract,
    UnsignedSubtractAdd,
    UnsignedSubtract16,
    UnsignedAdd8,
    UnsignedSubtract8,
    SignedAdd16,
    SignedAddSubtract,
    SignedSubtractAdd,
    SignedSubtract16,
    SignedAdd8,
    SignedSubtract8
};
constexpr ParallelArithmeticLayout parallel_saturating_layout(
    ParallelSaturatingOperation operation) noexcept
{
    // Both saturation families retain the ordinary parallel lane geometry.
    const auto shape =
        static_cast<unsigned>(operation) %
        static_cast<unsigned>(ParallelSaturatingOperation::SignedAdd16);
    return parallel_arithmetic_layout(
        static_cast<ParallelArithmeticOperation>(shape));
}
std::uint32_t parallel_saturating(
    const Instruction&, std::uint32_t first, std::uint32_t second) noexcept;
}
