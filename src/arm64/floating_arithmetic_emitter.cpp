/* SPDX-License-Identifier: MPL-2.0 */
#include "floating_emitter.hpp"
namespace ilemu::execution::arm64 {
template <class Scalar>
void FloatingEmitter::arithmetic_result(arm::FloatingArithmeticOperation op,
    Scalar first, Scalar second, Scalar accumulator)
{
    using Op = arm::FloatingArithmeticOperation;
    switch (op) {
    case Op::Add:
        code_.FADD(first, first, second);
        break;
    case Op::Subtract:
        code_.FSUB(first, first, second);
        break;
    case Op::Multiply:
        code_.FMUL(first, first, second);
        break;
    case Op::Divide:
        code_.FDIV(first, first, second);
        break;
    case Op::SquareRoot:
        code_.FSQRT(first, first);
        break;
    case Op::Move:
        break;
    case Op::Absolute:
        code_.FABS(first, first);
        break;
    case Op::Negate:
        code_.FNEG(first, first);
        break;
    default:
        // VFPv2 multiply-accumulate has two rounding/exception stages.
        code_.FMUL(first, first, second);
        if (op == Op::NegatedMultiply || op == Op::MultiplySubtract ||
            op == Op::NegatedMultiplyAdd)
            code_.FNEG(first, first);
        if (op == Op::NegatedMultiply)
            break;
        if (op == Op::NegatedMultiplyAdd || op == Op::NegatedMultiplySubtract)
            code_.FNEG(accumulator, accumulator);
        code_.FADD(first, accumulator, first);
        break;
    }
}
void FloatingEmitter::arithmetic(const arm::FloatingArithmeticOperands& v)
{
    using namespace oaknut;
    using namespace oaknut::util;
    const auto load = [&](QReg temporary, unsigned word) {
        if (v.wide)
            vectors_.read_d(temporary, word / 2U);
        else
            code_.DUP(
                temporary.S4(), vectors_.read(word / 2U).Selem()[word % 4U]);
    };
    load(Q0, v.first);
    using Op = arm::FloatingArithmeticOperation;
    if (v.operation != Op::SquareRoot && v.operation != Op::Move &&
        v.operation != Op::Absolute && v.operation != Op::Negate)
        load(Q1, v.second);
    if (v.operation == Op::MultiplyAdd || v.operation == Op::MultiplySubtract ||
        v.operation == Op::NegatedMultiplyAdd ||
        v.operation == Op::NegatedMultiplySubtract)
        load(Q2, v.destination);
    if (v.wide) {
        arithmetic_result(v.operation, D0, D1, D2);
        vectors_.write_d(v.destination / 2U, Q0);
    } else {
        arithmetic_result(v.operation, S0, S1, S2);
        vectors_.write_s(v.destination, Q0);
    }
}
}
