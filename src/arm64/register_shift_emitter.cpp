/* SPDX-License-Identifier: MPL-2.0 */
#include "integer_emitter.hpp"
namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
void IntegerEmitter::enter_shift_range()
{
    if (!shift_range_ || shift_range_->range != ShiftCountRange::Small)
        return;
    const auto kinds = shift_range_->carry_kinds;
    const WReg count { static_cast<int>(shift_range_->reg) };
    // These traces contain only ALU and their closing branch. They cannot
    // use the memory address cache's X27/X28, and the native frame/callback
    // ABI already preserves both. Backedges skip this initialization.
    if ((kinds & 1U) != 0)
        code_.ADD(W28, count, 31);
    if ((kinds & 6U) != 0)
        code_.NEG(W27, count);
    if ((kinds & 7U) != 0)
        code_.align(16);
}
void IntegerEmitter::bounded_shift(
    const arm::Instruction& inst, WReg destination, WReg source)
{
    if (shift_range_->range == ShiftCountRange::Zero) {
        code_.MOV(destination, source);
        return;
    }
    if (shift_range_->at_least_word() && inst.shift != arm::ShiftKind::Ror) {
        if (inst.shift == arm::ShiftKind::Asr)
            code_.ASR(destination, source, 31);
        else
            code_.MOV(destination, 0);
        return;
    }
    const WReg count { static_cast<int>(inst.rs) };
    switch (inst.shift) {
    case arm::ShiftKind::Lsl: code_.LSLV(destination, source, count); break;
    case arm::ShiftKind::Lsr: code_.LSRV(destination, source, count); break;
    case arm::ShiftKind::Asr: code_.ASRV(destination, source, count); break;
    case arm::ShiftKind::Ror: code_.RORV(destination, source, count); break;
    }
}
void IntegerEmitter::bounded_shifter(const arm::Instruction& inst,
    WReg destination, WReg source, bool carry_output, LogicalFlagOutput flags)
{
    if (!carry_output || shift_range_->range == ShiftCountRange::Zero) {
        bounded_shift(inst, destination, source);
        return; // Zero preserves incoming C, including high-only counts.
    }
    if (inst.shift == arm::ShiftKind::Ror) {
        bounded_shift(inst, destination, source);
        carry_from(destination, 31, flags);
        return;
    }
    if (shift_range_->range == ShiftCountRange::Small) {
        const WReg count { static_cast<int>(inst.rs) };
        if (flags == LogicalFlagOutput::CarryOnly) {
            XReg amount = inst.shift == arm::ShiftKind::Lsl ? X28 : X27;
            if ((shift_range_->carry_kinds &
                    (1U << static_cast<unsigned>(inst.shift))) == 0) {
                if (inst.shift == arm::ShiftKind::Lsl)
                    code_.ADD(W15, count, 31);
                else
                    code_.NEG(W15, count);
                amount = X15;
            }
            code_.LSLV(X15, source.toX(), amount);
            code_.ADDS(XZR, X15, X15);
        } else {
            if (inst.shift == arm::ShiftKind::Lsl)
                code_.NEG(W15, count);
            else
                code_.SUB(W15, count, 1);
            code_.LSRV(W15, source, W15);
            code_.BFI(X21, X15, 29, 1);
        }
    } else if (inst.shift == arm::ShiftKind::Asr ||
               shift_range_->range == ShiftCountRange::Word) {
        carry_from(source, inst.shift == arm::ShiftKind::Lsl ? 0 : 31, flags);
    } else if (flags == LogicalFlagOutput::CarryOnly)
        code_.CMN(WZR, WZR, AddSubShift::LSL, 0);
    else
        code_.BFI(X21, XZR, 29, 1);
    bounded_shift(inst, destination, source);
}
}
