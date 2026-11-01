/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_conversion.hpp"

namespace ilemu::arm {
bool decode_simd_conversion(Instruction& out, std::uint32_t word, bool thumb,
    execution::VfpRegisterProfile bank,
    execution::ArmInstructionProfile architecture,
    execution::AdvancedSimdProfile profile) noexcept
{
    if (thumb) {
        if ((word & 0xef000000U) != 0xef000000U)
            return false;
        word = 0xf2000000U | ((word >> 4U) & 0x01000000U) |
               (word & 0x00ffffffU);
    }
    if ((word & 0xffb30e10U) != 0xf3b30600U)
        return false;
    if (!thumb)
        out.condition = 14;
    VectorFloatingConversionOperands v;
    v.destination = ((word >> 18U) & 16U) | ((word >> 12U) & 15U);
    v.source = ((word >> 1U) & 16U) | (word & 15U);
    v.quad = (word & 64U) != 0;
    if (((word >> 18U) & 3U) != 2U ||
        profile != execution::AdvancedSimdProfile::Neon ||
        !execution::has_armv6t2_extensions(architecture) ||
        (v.quad && ((v.destination | v.source) & 1U) != 0) ||
        (bank == execution::VfpRegisterProfile::D16 &&
            (v.destination | v.source) >= 16U))
        return true;
    v.to_integer = (word & 256U) != 0;
    v.unsigned_value = (word & 128U) != 0;
    out.vector = v;
    out.kind = InstructionKind::VectorFloatingConversion;
    return true;
}
}
