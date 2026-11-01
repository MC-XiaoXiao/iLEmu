/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_reverse.hpp"

namespace ilemu::arm {
bool decode_simd_reverse(Instruction& out, std::uint32_t word, bool thumb,
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
    if ((word & 0xffb30e10U) != 0xf3b00000U)
        return false;
    if (!thumb)
        out.condition = 14;
    const auto size = (word >> 18U) & 3U;
    const auto operation = (word >> 7U) & 3U;
    VectorReverseOperands v;
    v.destination = ((word >> 18U) & 16U) | ((word >> 12U) & 15U);
    v.source = ((word >> 1U) & 16U) | (word & 15U);
    v.quad = (word & 64U) != 0;
    if (size + operation >= 3U ||
        profile != execution::AdvancedSimdProfile::Neon ||
        !execution::has_armv6t2_extensions(architecture) ||
        (v.quad && ((v.destination | v.source) & 1U) != 0) ||
        (bank == execution::VfpRegisterProfile::D16 &&
            (v.destination | v.source) >= 16U))
        return true;
    v.element_bits = 8U << size;
    v.block_bits = 64U >> operation;
    out.vector = v;
    out.kind = InstructionKind::VectorReverse;
    return true;
}
}
