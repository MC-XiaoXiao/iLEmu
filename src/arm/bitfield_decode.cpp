/* SPDX-License-Identifier: MPL-2.0 */
#include "bitfield_decode.hpp"
#include "arm/integer_transform.hpp"

namespace ilemu::arm {
bool decode_bitfield(Instruction& out, std::uint32_t word, bool thumb,
    execution::ArmInstructionProfile architecture) noexcept
{
    unsigned rd, source, lsb, field;
    bool insert, sign;
    if (thumb) {
        switch (word & 0xfff08020U) {
        case 0xf3600000U: insert = true; sign = false; break;
        case 0xf3400000U: insert = false; sign = true; break;
        case 0xf3c00000U: insert = false; sign = false; break;
        default: return false;
        }
        rd = (word >> 8U) & 15U;
        source = (word >> 16U) & 15U;
        lsb = ((word >> 10U) & 28U) | ((word >> 6U) & 3U);
        field = word & 31U;
        if (rd == 13 || source == 13)
            return true;
    } else {
        if (out.condition == 15)
            return false;
        switch (word & 0x0fe00070U) {
        case 0x07c00010U: insert = true; sign = false; break;
        case 0x07a00050U: insert = false; sign = true; break;
        case 0x07e00050U: insert = false; sign = false; break;
        default: return false;
        }
        rd = (word >> 12U) & 15U;
        source = word & 15U;
        lsb = (word >> 7U) & 31U;
        field = (word >> 16U) & 31U;
    }
    if (!execution::has_armv6t2_extensions(architecture) || rd == 15 ||
        (!insert && source == 15) || (insert ? field < lsb : field + lsb >= 32))
        return true;
    out.kind = InstructionKind::IntegerTransform;
    out.opcode = static_cast<unsigned>(insert
            ? IntegerTransformOperation::BitfieldInsert
            : IntegerTransformOperation::BitfieldExtract);
    // BFC is the PC-source alias of BFI; it reads only the old destination.
    out.rm = source == 15 ? 0U : source;
    out.rn = rd;
    out.rd = rd;
    out.accumulate = insert && source != 15;
    out.sign_extend = sign;
    out.shift_amount = lsb;
    out.immediate = insert ? field - lsb + 1U : field + 1U;
    return true;
}
}
