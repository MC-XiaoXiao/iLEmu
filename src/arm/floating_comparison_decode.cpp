/* SPDX-License-Identifier: MPL-2.0 */
#include "floating_comparison_decode.hpp"
namespace ilemu::arm {
bool decode_floating_comparison(Instruction& out, std::uint32_t word,
    bool thumb, execution::VfpRegisterProfile bank,
    execution::ArmInstructionProfile architecture) noexcept
{
    if ((thumb ? (word >> 28U) != 14U : out.condition == 15) ||
        (word & 0x0fbe0e50U) != 0x0eb40a40U)
        return false;
    FloatingComparisonOperands v;
    v.wide = (word & 256U) != 0;
    v.with_zero = (word & (1U << 16U)) != 0;
    v.signal_all_nans = (word & 128U) != 0;
    const auto reg = [&](unsigned low, unsigned high) {
        return v.wide ? (low + high * 16U) * 2U : low * 2U + high;
    };
    v.first = reg((word >> 12U) & 15U, (word >> 22U) & 1U);
    v.second = reg(word & 15U, (word >> 5U) & 1U);
    if ((v.with_zero && (word & 0x2fU) != 0) ||
        (thumb && !execution::has_armv6t2_extensions(architecture)) ||
        (bank == execution::VfpRegisterProfile::D16 && v.wide &&
            (v.first >= 32 || (!v.with_zero && v.second >= 32))))
        return true;
    out.kind = InstructionKind::FloatingComparison;
    out.vector = v;
    return true;
}
}
