/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_duplicate.hpp"
#include <bit>

namespace ilemu::arm {
bool decode_simd_duplicate(
    Instruction& out, std::uint32_t word, bool thumb) noexcept
{
    VectorDuplicateOperands v;
    if ((word & (thumb ? 0xff900f5fU : 0x0f900f5fU)) ==
            (thumb ? 0xee800b10U : 0x0e800b10U) &&
        (thumb || out.condition != 15)) {
        v.core_source = true;
        v.quad = (word & (1U << 21U)) != 0;
        v.destination = ((word >> 3U) & 16U) | ((word >> 16U) & 15U);
        v.source = (word >> 12U) & 15U;
        const auto size = ((word >> 21U) & 2U) | ((word >> 5U) & 1U);
        if (size == 3 || v.source == 15 || (thumb && v.source == 13) ||
            (v.quad && (v.destination & 1U) != 0))
            return true;
        v.element_bits = size == 0 ? 32U : size == 1 ? 16U : 8U;
    } else if ((word & 0xffb00f90U) ==
               (thumb ? 0xffb00c00U : 0xf3b00c00U)) {
        v.quad = (word & 64U) != 0;
        v.destination = ((word >> 18U) & 16U) | ((word >> 12U) & 15U);
        v.source = ((word >> 1U) & 16U) | (word & 15U);
        const auto imm = (word >> 16U) & 15U;
        if ((imm & 7U) == 0 || (v.quad && (v.destination & 1U) != 0))
            return true;
        const auto size = std::countr_zero(imm);
        v.element_bits = 8U << size;
        v.lane = imm >> (size + 1U);
        if (!thumb)
            out.condition = 14;
    } else
        return false;
    out.kind = InstructionKind::VectorDuplicate;
    out.vector = v;
    return true;
}
}
