/* SPDX-License-Identifier: MPL-2.0 */
#include "integer_emitter.hpp"
#include "arm/integer_transform.hpp"

namespace ilemu::execution::arm64 {
void IntegerEmitter::transform(const arm::Instruction& inst)
{
    using namespace oaknut;
    using namespace oaknut::util;
    using Operation = arm::IntegerTransformOperation;
    const auto operation = static_cast<Operation>(inst.opcode);
    const WReg d { static_cast<int>(inst.rd) }, m { static_cast<int>(inst.rm) },
        n { static_cast<int>(inst.rn) };
    switch (operation) {
    case Operation::BitfieldInsert:
        // Encode the inclusive end directly: the alias API cannot represent
        // width 32 in its five-bit width argument.
        code_.BFM(d, inst.accumulate ? m : WZR,
            (32U - inst.shift_amount) & 31U, inst.immediate - 1U);
        return;
    case Operation::BitfieldExtract:
        if (inst.sign_extend)
            code_.SBFM(d, m, inst.shift_amount, inst.shift_amount + inst.immediate - 1U);
        else
            code_.UBFM(d, m, inst.shift_amount, inst.shift_amount + inst.immediate - 1U);
        return;
    case Operation::CountLeadingZeros: code_.CLZ(d, m); return;
    case Operation::ReverseBytes: code_.REV(d, m); return;
    case Operation::ReverseHalfwords: code_.REV16(d, m); return;
    case Operation::ReverseSignedHalfword:
        code_.REV16(d, m);
        code_.SXTH(d, d);
        return;
    case Operation::ReverseBits: code_.RBIT(d, m); return;
    default: break;
    }
    const auto bits = operation == Operation::ExtendHalfword ? 16U : 8U;
    // Most rotated scalar extensions are one bitfield instruction, including
    // aliased destinations. Only a halfword rotated by24 wraps bit31.
    if (operation != Operation::ExtendBytePair && !inst.accumulate &&
        inst.shift_amount + bits <= 32) {
        if (inst.sign_extend)
            code_.SBFX(d, m, inst.shift_amount, bits);
        else
            code_.UBFX(d, m, inst.shift_amount, bits);
        return;
    }
    auto source = m;
    if (inst.shift_amount != 0) {
        code_.ROR(W16, m, inst.shift_amount);
        source = W16;
    }
    if (operation != Operation::ExtendBytePair) {
        if (inst.accumulate) {
            const auto extension = inst.sign_extend
                ? (bits == 8 ? AddSubExt::SXTB : AddSubExt::SXTH)
                : (bits == 8 ? AddSubExt::UXTB : AddSubExt::UXTH);
            code_.ADD(d, n, source, extension);
        } else if (inst.sign_extend)
            code_.SXTH(d, source);
        else
            code_.UXTH(d, source);
        return;
    }
    if (inst.sign_extend) {
        // Read the high source before extending an aliased low destination.
        code_.SBFX(W17, source, 16, 8);
        code_.SXTB(W16, source);
        code_.BFI(W16, W17, 16, 16);
    } else
        code_.AND(W16, source, 0x00ff00ffU);
    if (inst.accumulate) {
        code_.ADD(W17, n, W16);
        code_.LSR(W21, W16, 16);
        code_.ADD(W21, W21, n, AddSubShift::LSR, 16);
        code_.BFI(W17, W21, 16, 16);
        code_.MOV(d, W17);
    } else
        code_.MOV(d, W16);
}
}
