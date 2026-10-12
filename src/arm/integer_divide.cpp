/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/integer_divide.hpp"
#include "integer_divide_decode.hpp"
#include <bit>

namespace ilemu::arm {
bool decode_integer_divide(Instruction& out, std::uint32_t word, bool thumb,
    execution::ArmInstructionProfile architecture,
    execution::IntegerDivideProfile profile) noexcept
{
    unsigned rd, rn, rm;
    bool sign;
    if (thumb) {
        if ((word & 0xffd0f0f0U) != 0xfb90f0f0U)
            return false;
        rd = (word >> 8U) & 15U;
        rn = (word >> 16U) & 15U;
        rm = word & 15U;
        if (rd == 13 || rn == 13 || rm == 13)
            return true;
        sign = (word & 0x00200000U) == 0;
    } else {
        if (out.condition == 15 || (word & 0x0fd0f0f0U) != 0x0710f010U)
            return false;
        rd = (word >> 16U) & 15U;
        rn = word & 15U;
        rm = (word >> 8U) & 15U;
        sign = (word & 0x00200000U) == 0;
    }
    if (!execution::has_armv6t2_extensions(architecture) ||
        profile == execution::IntegerDivideProfile::None ||
        (!thumb && profile != execution::IntegerDivideProfile::ArmAndThumb) ||
        rd == 15 || rn == 15 || rm == 15)
        return true;
    out.kind = InstructionKind::IntegerDivide;
    out.rd = rd;
    out.rn = rn;
    out.rm = rm;
    out.sign_extend = sign;
    return true;
}
std::uint32_t integer_divide(const Instruction& inst, std::uint32_t dividend,
    std::uint32_t divisor) noexcept
{
    if (divisor == 0)
        return 0;
    if (!inst.sign_extend)
        return dividend / divisor;
    // Widen before division: INT_MIN / -1 must wrap without C++ overflow.
    return static_cast<std::uint32_t>(
        std::int64_t { std::bit_cast<std::int32_t>(dividend) } /
        std::bit_cast<std::int32_t>(divisor));
}
}
