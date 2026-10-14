/* SPDX-License-Identifier: MPL-2.0 */
#include "transform_decode.hpp"
#include "bitfield_decode.hpp"
#include "arm/integer_transform.hpp"

namespace ilemu::arm {
bool decode_integer_transform(Instruction& out, std::uint32_t word,
    bool thumb, execution::ArmInstructionProfile architecture) noexcept
{
    if (decode_bitfield(out, word, thumb, architecture))
        return true;
    using Operation = IntegerTransformOperation;
    Operation operation;
    unsigned rd, rm, rn = 15, rotation = 0;
    bool sign = false;
    if (thumb && word <= 0xffffU) {
        const auto op = (word >> 6U) & 3U;
        if ((word & 0xff00U) == 0xb200U) {
            operation = (op & 1U) != 0 ? Operation::ExtendByte : Operation::ExtendHalfword;
            sign = op < 2;
        } else if ((word & 0xff00U) == 0xba00U) {
            if (op == 2)
                return true; // reserved, not a short RBIT encoding
            operation = op == 0 ? Operation::ReverseBytes
                : op == 1 ? Operation::ReverseHalfwords : Operation::ReverseSignedHalfword;
        } else
            return false;
        rd = word & 7U;
        rm = (word >> 3U) & 7U;
    } else if (thumb) {
        const auto head = word >> 16U, tail = word & 0xffffU;
        if ((head & 0xff80U) == 0xfa00U && (tail & 0xf0c0U) == 0xf080U) {
            const auto op = (head >> 4U) & 7U;
            if (op >= 6)
                return false;
            operation = op < 2 ? Operation::ExtendHalfword
                : op < 4 ? Operation::ExtendBytePair : Operation::ExtendByte;
            sign = (op & 1U) == 0;
            rn = head & 15U;
            rotation = ((tail >> 4U) & 3U) * 8U;
        } else if (((head & 0xfff0U) == 0xfa90U &&
                       (tail & 0xf0c0U) == 0xf080U) ||
                   ((head & 0xfff0U) == 0xfab0U &&
                       (tail & 0xf0f0U) == 0xf080U)) {
            constexpr Operation operations[] = { Operation::ReverseBytes,
                Operation::ReverseHalfwords, Operation::ReverseBits,
                Operation::ReverseSignedHalfword };
            operation = (head & 0x20U) != 0 ? Operation::CountLeadingZeros
                                          : operations[(tail >> 4U) & 3U];
            if ((head & 15U) != (tail & 15U))
                return true;
        } else
            return false;
        if (!execution::has_armv6t2_extensions(architecture))
            return true; // These wide Thumb encodings require ARMv6T2.
        rd = (tail >> 8U) & 15U;
        rm = tail & 15U;
        if (rd == 13 || rm == 13 || rn == 13)
            return true; // ARMv7 Thumb data operands exclude SP
    } else {
        if (out.condition == 15)
            return false;
        // ARM PKHBT/PKHTB share the existing Thumb pack execution path.
        // Bit 6 selects LSL/ASR; a zero ASR field denotes a shift of 32.
        if ((word & 0x0ff00030U) == 0x06800010U) {
            out.rd = (word >> 12U) & 15U;
            out.rn = (word >> 16U) & 15U;
            out.rm = word & 15U;
            if (out.rd == 15 || out.rn == 15 || out.rm == 15)
                return true;
            out.kind = InstructionKind::PackHalfword;
            out.shift = (word & 0x40U) != 0 ? ShiftKind::Asr : ShiftKind::Lsl;
            out.shift_amount = (word >> 7U) & 31U;
            return true;
        }
        if ((word & 0x0f8003f0U) == 0x06800070U) {
            const auto op = (word >> 20U) & 7U;
            if (op == 1 || op == 5)
                return false;
            operation = (op & 3U) == 0 ? Operation::ExtendBytePair
                : (op & 3U) == 2 ? Operation::ExtendByte : Operation::ExtendHalfword;
            sign = op < 4;
            rn = (word >> 16U) & 15U;
            rotation = ((word >> 10U) & 3U) * 8U;
        } else {
            switch (word & 0x0fff0ff0U) {
            case 0x016f0f10U: operation = Operation::CountLeadingZeros; break;
            case 0x06bf0f30U: operation = Operation::ReverseBytes; break;
            case 0x06bf0fb0U: operation = Operation::ReverseHalfwords; break;
            case 0x06ff0fb0U: operation = Operation::ReverseSignedHalfword; break;
            case 0x06ff0f30U: operation = Operation::ReverseBits; break;
            default: return false;
            }
            if (operation == Operation::ReverseBits &&
                !execution::has_armv6t2_extensions(architecture))
                return true;
        }
        rd = (word >> 12U) & 15U;
        rm = word & 15U;
    }
    if (rd == 15 || rm == 15)
        return true;
    out.kind = InstructionKind::IntegerTransform;
    out.opcode = static_cast<unsigned>(operation);
    out.rd = rd;
    out.rm = rm;
    out.rn = rn == 15 ? 0U : rn;
    out.accumulate = rn != 15;
    out.sign_extend = sign;
    out.shift_amount = rotation;
    return true;
}
}
