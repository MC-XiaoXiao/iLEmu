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
    bool scalar = false;
    switch (word & 0xffa00f10U) {
    case 0xf2000d00U: v.operation = Op::Add; break;
    case 0xf2200d00U: v.operation = Op::Subtract; break;
    case 0xf3000d10U: v.operation = Op::Multiply; break;
    case 0xf2000d10U: v.operation = Op::MultiplyAdd; break;
    case 0xf2200d10U: v.operation = Op::MultiplySubtract; break;
    case 0xf3000d00U:
        v.operation = Op::Add;
        v.pairwise = true;
        break;
    default:
        // Two registers and an F32 scalar. Q occupies the ARM U bit;
        // bit M selects the scalar lane rather than a high D register.
        // The F32 size field distinguishes scalar arithmetic from VEXT.
        switch (word & 0xfeb00f50U) {
        case 0xf2a00140U: v.operation = Op::MultiplyAdd; break;
        case 0xf2a00540U: v.operation = Op::MultiplySubtract; break;
        case 0xf2a00940U: v.operation = Op::Multiply; break;
        default: return false;
        }
        scalar = true;
        break;
    }
    if (!thumb)
        out.condition = 14;
    // These encodings have only 32-bit floating-point elements in NEON.
    const bool single_precision = scalar
        ? ((word >> 20U) & 3U) == 2U : (word & 0x00100000U) == 0;
    if (!single_precision ||
        profile != execution::AdvancedSimdProfile::Neon ||
        !execution::has_armv6t2_extensions(architecture))
        return true;
    v.destination = ((word >> 18U) & 16U) | ((word >> 12U) & 15U);
    v.first = ((word >> 3U) & 16U) | ((word >> 16U) & 15U);
    if (scalar) {
        v.second = word & 15U;
        v.scalar_lane = static_cast<std::uint8_t>((word >> 5U) & 1U);
        v.quad = (word & 0x01000000U) != 0;
    } else {
        v.second = ((word >> 1U) & 16U) | (word & 15U);
        v.quad = (word & 64U) != 0;
    }
    const auto registers = v.destination | v.first | v.second;
    const auto vectors = v.destination | v.first | (scalar ? 0U : v.second);
    if ((v.quad && ((vectors & 1U) != 0 || v.pairwise)) ||
        (bank == execution::VfpRegisterProfile::D16 && registers >= 16U))
        return true;
    out.vector = v;
    out.kind = InstructionKind::VectorFloatingArithmetic;
    return true;
}
}
