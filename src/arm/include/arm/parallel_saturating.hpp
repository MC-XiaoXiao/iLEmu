/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
namespace ilemu::arm {
// ParallelSaturating uses opcode for the operation and rd/rn/rm for GPRs.
enum class ParallelSaturatingOperation {
    UnsignedAdd16,
    UnsignedAddSubtract,
    UnsignedSubtractAdd,
    UnsignedSubtract16,
    UnsignedAdd8,
    UnsignedSubtract8
};
std::uint32_t parallel_saturating(
    const Instruction&, std::uint32_t first, std::uint32_t second) noexcept;
}
