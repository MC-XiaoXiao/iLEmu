/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_floating_decode.hpp"

namespace ilemu::arm {
bool decode_simd_floating(Instruction& out, std::uint32_t word, bool thumb,
    execution::VfpRegisterProfile bank,
    execution::ArmInstructionProfile architecture,
    execution::AdvancedSimdProfile profile) noexcept
{
    if (thumb) {
        if ((word & 0xef000000U) != 0xef000000U)
            return false;
        // Thumb moves the ARM U bit from bit 24 to bit 28.
        word = 0xf2000000U | ((word >> 4U) & 0x01000000U) |
               (word & 0x00ffffffU);
    }
    VectorFloatingArithmeticOperands v;
    using Op = FloatingArithmeticOperation;
    switch (word & 0xffa00f10U) {
    case 0xf2000d00U: v.operation = Op::Add; break;
    case 0xf2200d00U: v.operation = Op::Subtract; break;
    case 0xf3000d10U: v.operation = Op::Multiply; break;
    default: return false;
    }
    if (!thumb)
        out.condition = 14;
    // These encodings have only 32-bit floating-point elements in NEON.
    if ((word & 0x00100000U) != 0 ||
        profile != execution::AdvancedSimdProfile::Neon ||
        !execution::has_armv6t2_extensions(architecture))
        return true;
    v.destination = ((word >> 18U) & 16U) | ((word >> 12U) & 15U);
    v.first = ((word >> 3U) & 16U) | ((word >> 16U) & 15U);
    v.second = ((word >> 1U) & 16U) | (word & 15U);
    v.quad = (word & 64U) != 0;
    const auto registers = v.destination | v.first | v.second;
    if ((v.quad && (registers & 1U) != 0) ||
        (bank == execution::VfpRegisterProfile::D16 && registers >= 16U))
        return true;
    out.vector = v;
    out.kind = InstructionKind::VectorFloatingArithmetic;
    return true;
}
}
