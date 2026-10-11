/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_extract.hpp"

namespace ilemu::arm {
bool decode_simd_extract(
    Instruction& out, std::uint32_t word, bool thumb) noexcept
{
    if ((word & 0xffb00010U) != (thumb ? 0xefb00000U : 0xf2b00000U))
        return false;
    VectorExtractOperands v;
    v.destination = ((word >> 18U) & 16U) | ((word >> 12U) & 15U);
    v.first = ((word >> 3U) & 16U) | ((word >> 16U) & 15U);
    v.second = ((word >> 1U) & 16U) | (word & 15U);
    v.byte_offset = (word >> 8U) & 15U;
    v.quad = (word & 64U) != 0;
    if (v.quad ? ((v.destination | v.first | v.second) & 1U) != 0
               : v.byte_offset >= 8U)
        return true;
    if (!thumb)
        out.condition = 14;
    out.vector = v;
    out.kind = InstructionKind::VectorExtract;
    return true;
}
}
