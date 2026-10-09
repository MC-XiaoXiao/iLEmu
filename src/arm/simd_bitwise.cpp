/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_bitwise.hpp"
#include <array>

namespace ilemu::arm {
bool decode_simd_bitwise(
    Instruction& out, std::uint32_t word, bool thumb) noexcept
{
    VectorBitwiseOperands v;
    if ((word & (thumb ? 0xef800f10U : 0xfe800f10U)) ==
        (thumb ? 0xef000110U : 0xf2000110U)) {
        constexpr std::array operations {
            VectorBitwiseOperation::And, VectorBitwiseOperation::BitClear,
            VectorBitwiseOperation::Or, VectorBitwiseOperation::OrNot,
            VectorBitwiseOperation::Xor, VectorBitwiseOperation::Select,
            VectorBitwiseOperation::InsertIfTrue,
            VectorBitwiseOperation::InsertIfFalse
        };
        const auto op = ((word >> (thumb ? 26U : 22U)) & 4U) |
                        ((word >> 20U) & 3U);
        v.operation = operations[op];
        v.first = ((word >> 3U) & 16U) | ((word >> 16U) & 15U);
    } else if ((word & 0xffb30f90U) ==
               (thumb ? 0xffb00580U : 0xf3b00580U)) {
        if ((word & 0x000c0000U) != 0)
            return true;
        v.operation = VectorBitwiseOperation::Not;
    } else
        return false;
    v.destination = ((word >> 18U) & 16U) | ((word >> 12U) & 15U);
    v.second = ((word >> 1U) & 16U) | (word & 15U);
    v.quad = (word & 64U) != 0;
    if (v.quad && ((v.destination | v.first | v.second) & 1U) != 0)
        return true;
    if (!thumb)
        out.condition = 14;
    out.vector = v;
    out.kind = InstructionKind::VectorBitwise;
    return true;
}
}
