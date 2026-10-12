/* SPDX-License-Identifier: MPL-2.0 */
#include "program_status_decode.hpp"
#include <bit>
namespace ilemu::arm {
bool decode_program_status(Instruction& out, std::uint32_t word, bool thumb,
    execution::ArmInstructionProfile architecture) noexcept
{
    bool spsr;
    if (thumb) {
        if ((word & 0xffeff0ffU) == 0xf3ef8000U) {
            out.load = true;
            out.rd = (word >> 8U) & 15U;
        } else if ((word & 0xffe0f0ffU) == 0xf3808000U) {
            out.rd = (word >> 16U) & 15U;
            out.opcode = (word >> 8U) & 15U;
        } else
            return false;
        spsr = (word & (1U << 20U)) != 0;
        if (!execution::has_armv6t2_extensions(architecture) || out.rd == 13)
            return true;
    } else {
        if (out.condition == 15)
            return false;
        if ((word & 0x0fbf0fffU) == 0x010f0000U) {
            out.load = true;
            out.rd = (word >> 12U) & 15U;
        } else if ((word & 0x0fb0fff0U) == 0x0120f000U) {
            out.rd = word & 15U;
            out.opcode = (word >> 16U) & 15U;
        } else if ((word & 0x0fb0f000U) == 0x0320f000U) {
            out.opcode = (word >> 16U) & 15U;
            if (out.opcode == 0 && (word & (1U << 22U)) == 0)
                return false; // The separate architectural hint space.
            out.immediate_operand = true;
            out.immediate = std::rotr(word & 255U,
                static_cast<int>(((word >> 8U) & 15U) * 2U));
        } else
            return false;
        spsr = (word & (1U << 22U)) != 0;
    }
    // Independent execution binds User-mode state, which has no SPSR.
    if (spsr || (!out.immediate_operand && out.rd == 15) ||
        (!out.load && out.opcode == 0))
        return true;
    out.kind = InstructionKind::SystemControl;
    out.system = SystemOperation::ProgramStatusRegister;
    return true;
}
}
