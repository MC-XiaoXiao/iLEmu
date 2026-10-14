/* SPDX-License-Identifier: MPL-2.0 */
#include "floating_emitter.hpp"
namespace ilemu::execution::arm64 {
template <class Scalar>
void FloatingEmitter::arithmetic_result(arm::FloatingArithmeticOperation op,
    Scalar destination, Scalar first, Scalar second)
{
    using Op = arm::FloatingArithmeticOperation;
    switch (op) {
    case Op::Add: code_.FADD(destination, first, second); break;
    case Op::Subtract: code_.FSUB(destination, first, second); break;
    case Op::Multiply: code_.FMUL(destination, first, second); break;
    case Op::Divide: code_.FDIV(destination, first, second); break;
    case Op::SquareRoot: code_.FSQRT(destination, first); break;
    case Op::Move: code_.FMOV(destination, first); break;
    case Op::Absolute: code_.FABS(destination, first); break;
    case Op::Negate: code_.FNEG(destination, first); break;
    default:
        // Legacy multiply-accumulate rounds the product before the addition.
        code_.FMUL(destination, first, second);
        if (op == Op::NegatedMultiply || op == Op::MultiplySubtract ||
            op == Op::NegatedMultiplyAdd)
            code_.FNEG(destination, destination);
        break;
    }
}
void FloatingEmitter::arithmetic(const arm::FloatingArithmeticOperands& v)
{
    using namespace oaknut;
    using namespace oaknut::util;
    using Op = arm::FloatingArithmeticOperation;
    if (v.operation == Op::Move) {
        // A lane copy preserves all neighbouring architectural lanes without
        // first materializing the scalar in a temporary vector register.
        if (v.wide)
            code_.INS(vectors_.write(v.destination / 2U).Delem()[(v.destination / 2U) % 2U],
                vectors_.read(v.first / 2U).Delem()[(v.first / 2U) % 2U]);
        else
            code_.INS(vectors_.write(v.destination / 2U).Selem()[v.destination % 4U],
                vectors_.read(v.first / 2U).Selem()[v.first % 4U]);
        return;
    }
    const bool unary = v.operation == Op::SquareRoot ||
        v.operation == Op::Absolute || v.operation == Op::Negate;
    const bool accumulate = v.operation == Op::MultiplyAdd ||
        v.operation == Op::MultiplySubtract || v.operation == Op::NegatedMultiplyAdd ||
        v.operation == Op::NegatedMultiplySubtract;
    const auto emit = [&](auto destination, const auto& scalar) {
        const auto first = scalar(v.first, Q1);
        const auto second = unary || v.first == v.second
            ? first : scalar(v.second, Q2);
        arithmetic_result(v.operation, destination, first, second);
        if (accumulate) {
            // The product is now in scratch V0. Inputs are no longer needed;
            // only now may V1 be reused for an upper-lane accumulator.
            auto accumulator = scalar(v.destination, Q1);
            if (v.operation == Op::NegatedMultiplyAdd ||
                v.operation == Op::NegatedMultiplySubtract) {
                const decltype(destination) temporary { 1 };
                code_.FNEG(temporary, accumulator);
                accumulator = temporary;
            }
            code_.FADD(destination, accumulator, destination);
        }
    };
    if (v.wide) {
        emit(D0, [&](unsigned word, QReg scratch) {
            return vectors_.scalar_d(word / 2U, scratch);
        });
        vectors_.write_d(v.destination / 2U, Q0);
    } else {
        emit(S0, [&](unsigned word, QReg scratch) {
            return vectors_.scalar_s(word, scratch);
        });
        vectors_.write_s(v.destination, Q0);
    }
}
}
