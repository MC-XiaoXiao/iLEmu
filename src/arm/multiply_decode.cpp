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
        case 0xfb800000U: sign = true; operation = Operation::Long; break;
        case 0xfba00000U: operation = Operation::Long; break;
        case 0xfbc00000U: sign = accumulate = true; operation = Operation::Long; break;
        case 0xfbe00000U: accumulate = true; operation = Operation::Long; break;
        case 0xfbe00060U: operation = Operation::DualAccumulateLong; break;
        default: return false;
        }
        if (operation == Operation::Long || operation == Operation::DualAccumulateLong) {
            rd = (word >> 12U) & 15U;
            rn = (word >> 8U) & 15U;
        }
        rm = (word >> 16U) & 15U;
        rs = word & 15U;
        if (profile == execution::ArmInstructionProfile::Armv6 ||
            rd == 13 || rn == 13 || rm == 13 || rs == 13)
            return true;
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
            if (profile == execution::ArmInstructionProfile::Armv6)
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
