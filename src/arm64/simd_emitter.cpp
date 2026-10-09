/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_emitter.hpp"

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
QReg SimdEmitter::source(QReg scratch, unsigned d, bool quad)
{
    if (quad)
        return vectors_.read(d);
    vectors_.read_d(scratch, d);
    return scratch;
}
void SimdEmitter::duplicate(const arm::VectorDuplicateOperands& v)
{
    const auto destination = v.quad ? vectors_.write(v.destination) : Q0;
    if (v.core_source) {
        const WReg value { static_cast<int>(v.source) };
        if (v.element_bits == 8)
            code_.DUP(destination.B16(), value);
        else if (v.element_bits == 16)
            code_.DUP(destination.H8(), value);
        else
            code_.DUP(destination.S4(), value);
    } else {
        const auto value = vectors_.read(v.source);
        const auto lane = v.lane + (v.source % 2U) * (64U / v.element_bits);
        if (v.element_bits == 8)
            code_.DUP(destination.B16(), value.Belem()[lane]);
        else if (v.element_bits == 16)
            code_.DUP(destination.H8(), value.Helem()[lane]);
        else
            code_.DUP(destination.S4(), value.Selem()[lane]);
    }
    if (!v.quad)
        vectors_.write_d(v.destination, destination);
}
void SimdEmitter::bitwise(const arm::VectorBitwiseOperands& v)
{
    using Op = arm::VectorBitwiseOperation;
    const auto destination = v.quad ? vectors_.write(v.destination) : Q0;
    if (v.operation == Op::Not) {
        const auto second = source(Q2, v.second, v.quad);
        code_.MVN(destination.B16(), second.B16());
    } else {
        const auto first = source(Q1, v.first, v.quad);
        const auto second = source(Q2, v.second, v.quad);
        switch (v.operation) {
        case Op::And:
            code_.AND(destination.B16(), first.B16(), second.B16());
            break;
        case Op::BitClear:
            code_.BIC(destination.B16(), first.B16(), second.B16());
            break;
        case Op::Or:
            code_.ORR(destination.B16(), first.B16(), second.B16());
            break;
        case Op::OrNot:
            code_.ORN(destination.B16(), first.B16(), second.B16());
            break;
        case Op::Xor:
            code_.EOR(destination.B16(), first.B16(), second.B16());
            break;
        case Op::Select:
        case Op::InsertIfTrue:
        case Op::InsertIfFalse:
            if (!v.quad)
                vectors_.read_d(destination, v.destination);
            if (v.operation == Op::Select)
                code_.BSL(destination.B16(), first.B16(), second.B16());
            else if (v.operation == Op::InsertIfTrue)
                code_.BIT(destination.B16(), first.B16(), second.B16());
            else
                code_.BIF(destination.B16(), first.B16(), second.B16());
            break;
        case Op::Not:
            break;
        }
    }
    if (!v.quad)
        vectors_.write_d(v.destination, destination);
}
}
