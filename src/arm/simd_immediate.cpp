/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/simd_immediate.hpp"
#include "simd_immediate_decode.hpp"

namespace ilemu::arm {
bool decode_simd_immediate(Instruction& out, std::uint32_t word, bool thumb,
    execution::VfpRegisterProfile bank,
    execution::ArmInstructionProfile architecture) noexcept
{
    if ((word & (thumb ? 0xefb80090U : 0xfeb80090U)) !=
        (thumb ? 0xef800010U : 0xf2800010U))
        return false;
    VectorImmediateOperands v;
    v.destination = ((word >> 18U) & 16U) | ((word >> 12U) & 15U);
    v.imm8 = static_cast<std::uint8_t>(((word >> (thumb ? 21U : 17U)) & 128U) |
                                       ((word >> 12U) & 112U) | (word & 15U));
    v.cmode = static_cast<std::uint8_t>((word >> 8U) & 15U);
    v.op = (word & 32U) != 0;
    v.quad = (word & 64U) != 0;
    if (!thumb)
        out.condition = 14;
    const bool zero_unpredictable =
        v.imm8 == 0 &&
        ((v.cmode >= 2 && v.cmode <= 7) || (v.cmode >= 10 && v.cmode <= 13));
    if (architecture == execution::ArmInstructionProfile::Armv6 ||
        (bank == execution::VfpRegisterProfile::D16 && v.destination >= 16) ||
        (v.quad && (v.destination & 1U) != 0) || (v.cmode == 15 && v.op) ||
        zero_unpredictable)
        return true;
    out.vector = v;
    out.kind = InstructionKind::VectorImmediate;
    return true;
}
std::uint64_t expand_simd_immediate(const VectorImmediateOperands& v) noexcept
{
    const std::uint32_t imm = v.imm8;
    std::uint32_t value;
    if (v.cmode < 8)
        value = imm << ((v.cmode >> 1U) * 8U);
    else if (v.cmode < 12) {
        value = imm << (((v.cmode >> 1U) & 1U) * 8U);
        value |= value << 16U;
    } else if (v.cmode < 14) {
        const auto shift = v.cmode == 12 ? 8U : 16U;
        value = (imm << shift) | ((1U << shift) - 1U);
    } else if (v.cmode == 14) {
        if (v.op) {
            std::uint64_t mask = 0;
            for (unsigned byte = 0; byte < 8; ++byte)
                if ((imm & (1U << byte)) != 0)
                    mask |= std::uint64_t { 255 } << (byte * 8U);
            return mask;
        }
        value = imm * 0x01010101U;
    } else {
        // Floating-point constants are bit patterns, with no FP exceptions.
        value = ((imm & 128U) << 24U) |
                ((imm & 64U) != 0 ? 0x3e000000U : 0x40000000U) |
                ((imm & 63U) << 19U);
    }
    return value | (std::uint64_t { value } << 32U);
}
}
