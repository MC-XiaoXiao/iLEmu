/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_transfer.hpp"

namespace ilemu::arm {
bool decode_simd_transfer(
    Instruction& out, std::uint32_t word, bool thumb) noexcept
{
    if ((word & 0xff100000U) != (thumb ? 0xf9000000U : 0xf4000000U))
        return false;
    VectorTransferOperands v;
    if ((word & (1U << 23U)) == 0) {
        const unsigned align = (word >> 4U) & 3U;
        switch ((word >> 8U) & 15U) {
        case 7:
            v.count = 1;
            break;
        case 10:
            v.count = 2;
            break;
        case 6:
            v.count = 3;
            break;
        case 2:
            v.count = 4;
            break;
        default:
            return false;
        }
        if (((v.count == 1 || v.count == 3) && align >= 2) ||
            (v.count == 2 && align == 3))
            return true;
        v.element_size = 1U << ((word >> 6U) & 3U);
        v.alignment = align == 0 ? 1U : 4U << align;
    } else {
        if ((word & 0x300U) != 0)
            return false; // Interleaved structures use a separate family.
        const auto size = (word >> 10U) & 3U;
        const auto index = (word >> 4U) & 15U;
        v.count = 1;
        if (size == 3) {
            if ((word & (1U << 21U)) == 0)
                return true; // No replicate store encoding.
            const auto element = (word >> 6U) & 3U;
            if (element == 3 || (element == 0 && (index & 1U) != 0))
                return true;
            v.mode = VectorTransferMode::Replicate;
            v.element_size = 1U << element;
            v.count = (index & 2U) != 0 ? 2U : 1U;
            v.alignment = (index & 1U) != 0 ? v.element_size : 1U;
        } else {
            v.mode = VectorTransferMode::Lane;
            v.element_size = 1U << size;
            if ((size == 0 && (index & 1U) != 0) ||
                (size == 1 && (index & 2U) != 0) ||
                (size == 2 && ((index & 4U) != 0 ||
                                  ((index & 3U) != 0 && (index & 3U) != 3))))
                return true;
            v.lane = index >> (size + 1U);
            v.alignment = (index & 1U) != 0 ? v.element_size : 1U;
        }
    }
    v.first = ((word >> 18U) & 16U) | ((word >> 12U) & 15U);
    out.rn = (word >> 16U) & 15U;
    out.rm = word & 15U;
    if (out.rn == 15 || v.first + v.count > 32)
        return true;
    out.load = (word & (1U << 21U)) != 0;
    out.writeback = out.rm != 15;
    out.vector = v;
    if (!thumb)
        out.condition = 14;
    out.kind = InstructionKind::VectorTransfer;
    return true;
}
}
