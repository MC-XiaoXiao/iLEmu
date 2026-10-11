/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_emitter.hpp"

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
void emit_simd_immediate(VectorCodeGenerator& code, QReg destination,
    const arm::VectorImmediateOperands& v)
{
    const bool modify = v.cmode < 12 && (v.cmode & 1U) != 0;
    if (v.cmode < 8) {
        const int shift = static_cast<int>(v.cmode >> 1U) * 8;
        if (modify) {
            if (v.op)
                code.BIC(destination.S4(), v.imm8, LSL, shift);
            else
                code.ORR(destination.S4(), v.imm8, LSL, shift);
        } else if (v.op)
            code.MVNI(destination.S4(), v.imm8, LSL, shift);
        else
            code.MOVI(destination.S4(), v.imm8, LSL, shift);
    } else if (v.cmode < 12) {
        const int shift = static_cast<int>((v.cmode >> 1U) & 1U) * 8;
        if (modify) {
            if (v.op)
                code.BIC(destination.H8(), v.imm8, LSL, shift);
            else
                code.ORR(destination.H8(), v.imm8, LSL, shift);
        } else if (v.op)
            code.MVNI(destination.H8(), v.imm8, LSL, shift);
        else
            code.MOVI(destination.H8(), v.imm8, LSL, shift);
    } else if (v.cmode < 14) {
        const int shift = v.cmode == 12 ? 8 : 16;
        if (v.op)
            code.MVNI(destination.S4(), v.imm8, MSL, shift);
        else
            code.MOVI(destination.S4(), v.imm8, MSL, shift);
    } else if (v.cmode == 14) {
        if (v.op)
            code.MOVI(destination.D2(), RepImm { v.imm8 });
        else
            code.MOVI(destination.B16(), v.imm8);
    } else
        code.FMOV(destination.S4(), FImm8 { v.imm8 });
}
void SimdEmitter::immediate(const arm::VectorImmediateOperands& v)
{
    const bool modify = v.cmode < 12 && (v.cmode & 1U) != 0;
    if (!v.quad) {
        if (modify) {
            auto mask = v;
            mask.cmode = static_cast<std::uint8_t>(mask.cmode & ~1U);
            mask.op = false;
            const auto literal =
                vectors_.simd_constant(mask, v.destination % 2U != 0);
            const auto destination = vectors_.write(v.destination);
            if (v.op)
                code_.BIC(destination.B16(), destination.B16(), literal.B16());
            else
                code_.ORR(destination.B16(), destination.B16(), literal.B16());
        } else {
            const auto literal = vectors_.simd_constant(v, false);
            vectors_.write_d(v.destination, literal);
        }
        return;
    }
    emit_simd_immediate(code_, vectors_.write(v.destination), v);
}
}
