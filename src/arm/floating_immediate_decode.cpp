/* SPDX-License-Identifier: MPL-2.0 */
#include "floating_immediate_decode.hpp"
namespace ilemu::arm {
bool decode_floating_immediate(Instruction& out, std::uint32_t word,
    bool thumb, execution::VfpRegisterProfile bank,
    execution::ArmInstructionProfile architecture,
    execution::VfpInstructionProfile floating_instructions) noexcept
{
    if ((thumb ? (word >> 28U) != 14U : out.condition == 15) ||
        (word & 0x0fb00ef0U) != 0x0eb00a00U)
        return false;
    FloatingImmediateOperands v;
    v.wide = (word & 256U) != 0;
    const auto low = (word >> 12U) & 15U, high = (word >> 22U) & 1U;
    v.destination = v.wide ? (low + high * 16U) * 2U : low * 2U + high;
    v.imm8 = static_cast<std::uint8_t>(((word >> 12U) & 0xf0U) | (word & 15U));
    if (floating_instructions == execution::VfpInstructionProfile::Vfpv2 ||
        (thumb && !execution::has_armv6t2_extensions(architecture)) ||
        (v.wide && bank == execution::VfpRegisterProfile::D16 && v.destination >= 32))
        return true;
    out.kind = InstructionKind::FloatingImmediate;
    out.vector = v;
    return true;
}
}
