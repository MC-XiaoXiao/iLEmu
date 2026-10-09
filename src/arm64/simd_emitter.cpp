/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_emitter.hpp"
#include "execution/arm_state.hpp"
#include <cstddef>

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
void SimdEmitter::load(VReg reg, unsigned d, bool quad)
{
    const auto offset = offsetof(CpuThreadState, extension_registers) + d * 8U;
    if (quad)
        code_.LDR(reg.toQ(), X19, offset);
    else
        code_.LDR(reg.toD(), X19, offset);
}
void SimdEmitter::store(VReg reg, unsigned d, bool quad)
{
    const auto offset = offsetof(CpuThreadState, extension_registers) + d * 8U;
    if (quad)
        code_.STR(reg.toQ(), X19, offset);
    else
        code_.STR(reg.toD(), X19, offset);
}
void SimdEmitter::duplicate(const arm::VectorDuplicateOperands& v)
{
    // V0/V1 are caller-saved scratch. Guest D/Q values stay in architectural
    // storage, so poll callbacks and checked exits need no vector spill frame.
    if (v.core_source) {
        const WReg source { static_cast<int>(v.source) };
        if (v.element_bits == 8)
            code_.DUP(V0.B16(), source);
        else if (v.element_bits == 16)
            code_.DUP(V0.H8(), source);
        else
            code_.DUP(V0.S4(), source);
    } else {
        load(Q1, v.source, false);
        if (v.element_bits == 8)
            code_.DUP(V0.B16(), Q1.Belem()[v.lane]);
        else if (v.element_bits == 16)
            code_.DUP(V0.H8(), Q1.Helem()[v.lane]);
        else
            code_.DUP(V0.S4(), Q1.Selem()[v.lane]);
    }
    store(Q0, v.destination, v.quad);
}
void SimdEmitter::bitwise(const arm::VectorBitwiseOperands& v)
{
    using Op = arm::VectorBitwiseOperation;
    if (v.operation == Op::Or && v.first == v.second) {
        load(Q0, v.first, v.quad); // VMOV alias needs only one load.
    } else if (v.operation == Op::Not) {
        load(Q2, v.second, v.quad);
        code_.MVN(V0.B16(), V2.B16());
    } else {
        load(Q1, v.first, v.quad);
        load(Q2, v.second, v.quad);
        switch (v.operation) {
        case Op::And: code_.AND(V0.B16(), V1.B16(), V2.B16()); break;
        case Op::BitClear: code_.BIC(V0.B16(), V1.B16(), V2.B16()); break;
        case Op::Or: code_.ORR(V0.B16(), V1.B16(), V2.B16()); break;
        case Op::OrNot: code_.ORN(V0.B16(), V1.B16(), V2.B16()); break;
        case Op::Xor: code_.EOR(V0.B16(), V1.B16(), V2.B16()); break;
        case Op::Select:
        case Op::InsertIfTrue:
        case Op::InsertIfFalse:
            load(Q0, v.destination, v.quad);
            if (v.operation == Op::Select)
                code_.BSL(V0.B16(), V1.B16(), V2.B16());
            else if (v.operation == Op::InsertIfTrue)
                code_.BIT(V0.B16(), V1.B16(), V2.B16());
            else
                code_.BIF(V0.B16(), V1.B16(), V2.B16());
            break;
        case Op::Not: break;
        }
    }
    store(Q0, v.destination, v.quad);
}
}
