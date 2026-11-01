/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_shift_immediate.hpp"
#include <bit>

namespace ilemu::arm {
bool decode_simd_shift_immediate(Instruction& out, std::uint32_t word, bool thumb,
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
    if ((word & 0xfe800010U) != 0xf2800010U)
        return false;
    const auto operation = (word >> 8U) & 15U;
    const auto immediate = (word >> 16U) & 63U;
    const bool wide = (word & 128U) != 0;
    const bool u = (word & 0x01000000U) != 0;
    if (operation > 5 || (operation == 4 && !u) || (!wide && immediate < 8))
        return false;
    if (!thumb)
        out.condition = 14;
    VectorShiftImmediateOperands v;
    v.destination = ((word >> 18U) & 16U) | ((word >> 12U) & 15U);
    v.source = ((word >> 1U) & 16U) | (word & 15U);
    v.quad = (word & 64U) != 0;
    if (profile != execution::AdvancedSimdProfile::Neon ||
        !execution::has_armv6t2_extensions(architecture) ||
        (v.quad && ((v.destination | v.source) & 1U) != 0) ||
        (bank == execution::VfpRegisterProfile::D16 &&
            (v.destination | v.source) >= 16U))
        return true;
    v.element_bits = wide ? 64U : 1U << (std::bit_width(immediate) - 1U);
    v.right = operation != 5;
    v.unsigned_value = u;
    v.insert = operation == 4 || (operation == 5 && u);
    v.rounding = operation == 2 || operation == 3;
    v.accumulate = operation == 1 || operation == 3;
    v.amount = wide ? (v.right ? 64U - immediate : immediate)
                   : (v.right ? 2U * v.element_bits - immediate
                              : immediate - v.element_bits);
    out.vector = v;
    out.kind = InstructionKind::VectorShiftImmediate;
    return true;
}
}
