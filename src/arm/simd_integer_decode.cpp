/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_integer_decode.hpp"
namespace ilemu::arm {
bool decode_simd_integer(Instruction& out, std::uint32_t word, bool thumb,
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
    VectorIntegerOperands v;
    using Op = VectorIntegerOperation;
    switch (word & 0xff800f10U) {
    case 0xf2000800U: v.operation = Op::Add; break;
    case 0xf3000800U: v.operation = Op::Subtract; break;
    case 0xf2000910U: v.operation = Op::Multiply; break;
    case 0xf2000900U: v.operation = Op::MultiplyAdd; break;
    case 0xf3000900U: v.operation = Op::MultiplySubtract; break;
    default: return false;
    }
    if (!thumb)
        out.condition = 14;
    v.element_bits = 8U << ((word >> 20U) & 3U);
    if ((v.element_bits == 64 && v.operation != Op::Add &&
            v.operation != Op::Subtract) ||
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
    out.kind = InstructionKind::VectorIntegerArithmetic;
    return true;
}
}
