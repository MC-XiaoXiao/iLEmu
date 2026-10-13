/* SPDX-License-Identifier: MPL-2.0 */
#include "multiply_decode.hpp"
#include "arm/multiply.hpp"

namespace ilemu::arm {
bool decode_multiply(Instruction& out, std::uint32_t word, bool thumb,
    execution::ArmInstructionProfile profile, bool in_it_block) noexcept
{
    using Operation = MultiplyOperation;
    Operation operation = Operation::Word;
    unsigned rd, rn = 0, rm, rs;
    bool accumulate = false, flags = false, sign = false;
    if (thumb && word <= 0xffffU) {
        if ((word & 0xffc0U) != 0x4340U)
            return false;
        rd = rm = word & 7U;
        rs = (word >> 3U) & 7U;
        flags = !in_it_block;
    } else if (thumb) {
        const auto encoding = word & 0xfff000f0U;
        switch (encoding) {
        case 0xfb000000U:
        case 0xfb000010U:
            operation = encoding == 0xfb000010U ? Operation::Subtract : Operation::Word;
            rd = (word >> 8U) & 15U;
            rn = (word >> 12U) & 15U;
            accumulate = operation == Operation::Word && rn != 15;
            if (rn == 15) {
                if (operation == Operation::Subtract)
                    return true;
                rn = 0;
            }
            break;
        case 0xfb100000U:
        case 0xfb100010U:
        case 0xfb100020U:
        case 0xfb100030U:
            if (((word >> 12U) & 15U) != 15U)
                return false; // SMLAxy has a separate accumulator/Q contract.
            operation = Operation::Halfword;
            rd = (word >> 8U) & 15U;
            out.shift_amount = ((word >> 5U) & 1U) |
                (((word >> 4U) & 1U) << 1U);
            sign = true;
            break;
        case 0xfb800000U: sign = true; operation = Operation::Long; break;
        case 0xfba00000U: operation = Operation::Long; break;
        case 0xfbc00000U: sign = accumulate = true; operation = Operation::Long; break;
        case 0xfbe00000U: accumulate = true; operation = Operation::Long; break;
        case 0xfbe00060U: operation = Operation::DualAccumulateLong; break;
        case 0xfb500000U:
        case 0xfb500010U:
        case 0xfb600000U:
        case 0xfb600010U:
            operation = (word & 0x00f00000U) == 0x00600000U
                ? Operation::HighSubtract : Operation::High;
            rd = (word >> 8U) & 15U;
            rn = (word >> 12U) & 15U;
            accumulate = operation == Operation::HighSubtract || rn != 15;
            if (!accumulate)
                rn = 0;
            sign = true;
            out.shift_amount = (word >> 4U) & 1U;
            break;
        default: return false;
        }
        if (operation == Operation::Long || operation == Operation::DualAccumulateLong) {
            rd = (word >> 12U) & 15U;
            rn = (word >> 8U) & 15U;
        }
        rm = (word >> 16U) & 15U;
        rs = word & 15U;
        if (!execution::has_armv6t2_extensions(profile) ||
            rd == 13 || rn == 13 || rm == 13 || rs == 13)
            return true;
    } else if ((word & 0x0ff0f090U) == 0x01600080U) {
        if (out.condition == 15)
            return false;
        operation = Operation::Halfword;
        rd = (word >> 16U) & 15U;
        rm = word & 15U;
        rs = (word >> 8U) & 15U;
        out.shift_amount = ((word >> 5U) & 1U) |
            (((word >> 6U) & 1U) << 1U);
        sign = true;
    } else if ((word & 0x0ff000d0U) == 0x07500010U ||
               (word & 0x0ff000d0U) == 0x075000d0U) {
        if (out.condition == 15)
            return false;
        operation = (word & 0xc0U) != 0
            ? Operation::HighSubtract : Operation::High;
        rd = (word >> 16U) & 15U;
        rn = (word >> 12U) & 15U;
        rm = word & 15U;
        rs = (word >> 8U) & 15U;
        accumulate = operation == Operation::HighSubtract || rn != 15;
        if (!accumulate)
            rn = 0;
        sign = true;
        out.shift_amount = (word >> 5U) & 1U;
    } else {
        if ((word & 0x0f0000f0U) != 0x00000090U || out.condition == 15)
            return false;
        const auto op = (word >> 20U) & 15U;
        rm = word & 15U;
        rs = (word >> 8U) & 15U;
        if (op < 4) {
            rd = (word >> 16U) & 15U;
            rn = (word >> 12U) & 15U;
            accumulate = (op & 2U) != 0;
            flags = (op & 1U) != 0;
            if (!accumulate && rn != 0)
                return true;
        } else if (op == 6) {
            if (!execution::has_armv6t2_extensions(profile))
                return true;
            operation = Operation::Subtract;
            rd = (word >> 16U) & 15U;
            rn = (word >> 12U) & 15U;
        } else {
            if (op == 5 || op == 7)
                return true;
            operation = op == 4 ? Operation::DualAccumulateLong : Operation::Long;
            rd = (word >> 12U) & 15U;
            rn = (word >> 16U) & 15U;
            if (op >= 8) {
                sign = (op & 4U) != 0;
                accumulate = (op & 2U) != 0;
                flags = (op & 1U) != 0;
            }
        }
    }
    const bool pair = operation == Operation::Long || operation == Operation::DualAccumulateLong;
    if (rd == 15 || rn == 15 || rm == 15 || rs == 15 || (pair && rd == rn))
        return true;
    out.kind = InstructionKind::Multiply;
    out.opcode = static_cast<unsigned>(operation);
    out.rd = rd; out.rn = rn; out.rm = rm; out.rs = rs;
    out.accumulate = accumulate;
    out.set_flags = flags;
    out.sign_extend = sign;
    return true;
}
}
