/* SPDX-License-Identifier: MPL-2.0 */
#include "swap_decode.hpp"
namespace ilemu::arm {
bool decode_swap(Instruction& out, std::uint32_t word) noexcept
{
    if (out.condition == 15 || (word & 0x0fb00ff0U) != 0x01000090U)
        return false;
    out.rn = (word >> 16U) & 15U;
    out.rd = (word >> 12U) & 15U;
    out.rm = word & 15U;
    if (out.rn == 15 || out.rd == 15 || out.rm == 15 ||
        out.rn == out.rd || out.rn == out.rm)
        return true;
    out.kind = InstructionKind::Swap;
    out.access_size = (word & (1U << 22U)) != 0 ? 1U : 4U;
    return true;
}
}
