/* SPDX-License-Identifier: MPL-2.0 */
#include "system_control.hpp"
namespace ilemu::arm {
bool decode_system_control(
    Instruction& out, std::uint32_t word, bool thumb) noexcept
{
    const auto barrier = word & 0xfffffff0U;
    const auto base = thumb ? 0xf3bf8f00U : 0xf57ff000U;
    if (barrier == (base | 0x40U) || barrier == (base | 0x50U) ||
        barrier == (base | 0x60U)) {
        out.kind = InstructionKind::SystemControl;
        out.system = (word & 0xf0U) == 0x40U
                         ? SystemOperation::DataSynchronizationBarrier
                     : (word & 0xf0U) == 0x50U
                         ? SystemOperation::DataMemoryBarrier
                         : SystemOperation::InstructionSynchronizationBarrier;
        // All reserved/unsupported option values execute a full-system
        // barrier in ARMv7. Executors deliberately implement that stronger form.
        out.immediate = word & 15U;
        if (!thumb)
            out.condition = 14;
        return true;
    }
    // MCR/MRC p15, opc1=0. MCR2/MRC2 are not user register aliases.
    if ((word & 0x0fe00f10U) != 0x0e000f10U ||
        (thumb ? (word >> 28U) != 14 : out.condition == 15))
        return false;
    const auto n = (word >> 16U) & 15U, m = word & 15U;
    const auto op = (word >> 5U) & 7U;
    out.rd = (word >> 12U) & 15U;
    out.load = (word & (1U << 20U)) != 0;
    // CP15 forbids Rt=PC even for MRC-to-flags; Thumb also forbids SP.
    if (out.rd == 15 || (thumb && out.rd == 13))
        return true;
    if (n == 13 && m == 0 && (op == 2 || (op == 3 && out.load))) {
        out.system = op == 2 ? SystemOperation::ThreadPointerReadWrite
                            : SystemOperation::ThreadPointerReadOnly;
    } else if (!out.load && n == 7 &&
               ((m == 10 && (op == 4 || op == 5)) || (m == 5 && op == 4))) {
        out.system = m == 5 ? SystemOperation::InstructionSynchronizationBarrier
                     : op == 4 ? SystemOperation::DataSynchronizationBarrier
                               : SystemOperation::DataMemoryBarrier;
        out.immediate = 15;
    } else
        return true; // Privileged, reserved, or not implemented.
    out.kind = InstructionKind::SystemControl;
    return true;
}
}
