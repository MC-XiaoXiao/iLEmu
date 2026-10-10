/* SPDX-License-Identifier: MPL-2.0 */
#include "core_vector_move.hpp"

namespace ilemu::arm {
bool decode_core_vector_move(Instruction& out, std::uint32_t word, bool thumb,
    execution::VfpRegisterProfile vfp,
    execution::ArmInstructionProfile architecture) noexcept
{
    if (thumb ? (word >> 28U) != 14U : out.condition == 15)
        return false;
    VectorCoreMoveOperands v;
    v.core = (word >> 12U) & 15U;
    v.to_core = (word & (1U << 20U)) != 0;
    if ((word & 0x0fe00f7fU) == 0x0e000a10U) {
        v.first_word = ((word >> 16U) & 15U) * 2U + ((word >> 7U) & 1U);
    } else if ((word & 0x0fe00ed0U) == 0x0c400a10U) {
        v.pair = true;
        v.core2 = (word >> 16U) & 15U;
        const bool single = (word & 256U) == 0;
        const auto high = (word >> 5U) & 1U, low = word & 15U;
        v.first_word = single ? low * 2U + high : (high * 16U + low) * 2U;
        if ((single && v.first_word == 31) ||
            (!single && vfp == execution::VfpRegisterProfile::D16 &&
                v.first_word >= 32) ||
            (v.to_core && v.core == v.core2))
            return true;
    } else if ((word & 0x0f000f1fU) == 0x0e000b10U) {
        const bool unsigned_value = (word & (1U << 23U)) != 0;
        if (!v.to_core && unsigned_value)
            return false; // VDUP and other allocations, not a scalar move.
        const auto op1 = (word >> 21U) & 3U, op2 = (word >> 5U) & 3U;
        unsigned lane;
        if ((op1 & 2U) != 0) {
            v.element_bits = 8;
            lane = ((op1 & 1U) << 2U) | op2;
        } else if ((op2 & 1U) != 0) {
            v.element_bits = 16;
            lane = ((op1 & 1U) << 1U) | (op2 >> 1U);
        } else if (op2 == 0 && !unsigned_value) {
            lane = op1 & 1U;
        } else
            return true;
        const auto d = ((word >> 3U) & 16U) | ((word >> 16U) & 15U);
        if (vfp == execution::VfpRegisterProfile::D16 && d >= 16)
            return true;
        const auto bit = lane * v.element_bits;
        v.first_word = d * 2U + bit / 32U;
        v.bit_offset = bit % 32U;
        v.sign_extend = v.to_core && !unsigned_value && v.element_bits < 32;
    } else
        return false;
    if ((thumb && architecture == execution::ArmInstructionProfile::Armv6) ||
        v.core == 15 || (thumb && v.core == 13) ||
        (v.pair && (v.core2 == 15 || (thumb && v.core2 == 13))))
        return true;
    out.kind = InstructionKind::VectorCoreMove;
    out.vector = v;
    return true;
}
}
