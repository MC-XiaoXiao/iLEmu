/* SPDX-License-Identifier: MPL-2.0 */
#include "vfp_multiple.hpp"

namespace ilemu::arm {
bool decode_vfp_multiple(Instruction& out, std::uint32_t word, bool thumb,
    execution::VfpRegisterProfile profile) noexcept
{
    if ((word & 0x0e000e00U) != 0x0c000a00U ||
        (thumb ? (word >> 28U) != 14U : out.condition == 15))
        return false;
    const bool pre = (word & (1U << 24U)) != 0;
    const bool add = (word & (1U << 23U)) != 0;
    const bool writeback = (word & (1U << 21U)) != 0;
    // Other PUW combinations name VLDR/VSTR or core-register transfers.
    if ((!pre && !add && !writeback) || (pre && !writeback))
        return false;
    if (pre == add && writeback)
        return true; // UNDEFINED
    const auto rn = (word >> 16U) & 15U;
    const bool single = (word & (1U << 8U)) == 0;
    const auto high = (word >> 22U) & 1U, vd = (word >> 12U) & 15U;
    const auto first = single ? vd * 2U + high : high * 16U + vd;
    const auto immediate = word & 255U;
    const auto count = single ? immediate : immediate / 2U;
    if ((rn == 15 && (thumb || writeback)) || count == 0 ||
        first + count > 32 || (!single && (count > 16 ||
            ((profile == execution::VfpRegisterProfile::D16 ||
                 (immediate & 1U) != 0) && first + count > 16))))
        return true;
    out.kind = InstructionKind::VectorTransfer;
    out.rn = rn;
    out.load = (word & (1U << 20U)) != 0;
    out.add = add;
    out.index = pre;
    out.writeback = writeback;
    // Deprecated FLDMX/FSTMX odd counts skip the final word but include it in
    // address calculation/writeback, as specified by the ARMv7 pseudocode.
    out.immediate = immediate * 4U;
    out.vector = VectorTransferOperands { first, count, single ? 4U : 8U, 4U,
        VectorTransferMode::ScalarMultiple, 0U };
    return true;
}
}
