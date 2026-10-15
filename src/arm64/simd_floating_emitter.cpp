/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_emitter.hpp"

namespace ilemu::execution::arm64 {
void SimdEmitter::floating(const arm::VectorFloatingArithmeticOperands& v)
{
    using namespace oaknut::util;
    // Vector FP uses packed banks. Two-lane operations can read an even D
    // directly without copying its unrelated upper neighbour into scratch.
    const auto operand = [&](oaknut::QReg scratch, unsigned d) {
        return v.quad || d % 2U == 0 ? vectors_.read(d)
                                    : source(scratch, d, false);
    };
    const auto first = operand(Q1, v.first);
    const auto second = v.scalar_lane ? Q2 : operand(Q2, v.second);
    if (v.scalar_lane) {
        const auto bank = vectors_.read(v.second);
        code_.DUP(Q2.S4(), bank.Selem()[*v.scalar_lane + (v.second % 2U) * 2U]);
    }
    const auto destination = v.quad ? vectors_.write(v.destination) : Q0;
    using Op = arm::FloatingArithmeticOperation;
    if (v.pairwise) {
        code_.FADDP(Q0.toD().S2(), first.toD().S2(), second.toD().S2());
        vectors_.write_d(v.destination, Q0);
        return;
    }
    if (v.operation == Op::MultiplyAdd || v.operation == Op::MultiplySubtract) {
        // VMLA/VMLS round the product before adding it. FMLA would change
        // both the result and exception flags. Only V0..V2 are scratch.
        const auto product = v.quad ? Q0 : Q1;
        if (v.quad)
            code_.FMUL(product.S4(), first.S4(), second.S4());
        else {
            code_.FMUL(product.toD().S2(), first.toD().S2(), second.toD().S2());
            vectors_.read_d(Q0, v.destination);
        }
        const auto accumulate = [&](auto d, auto p) {
            if (v.operation == Op::MultiplyAdd)
                code_.FADD(d, d, p);
            else
                code_.FSUB(d, d, p);
        };
        if (v.quad)
            accumulate(destination.S4(), product.S4());
        else {
            accumulate(Q0.toD().S2(), product.toD().S2());
            vectors_.write_d(v.destination, Q0);
        }
        return;
    }
    const auto emit = [&](auto d, auto a, auto b) {
        if (v.operation == Op::Add)
            code_.FADD(d, a, b);
        else if (v.operation == Op::Subtract)
            code_.FSUB(d, a, b);
        else
            code_.FMUL(d, a, b);
    };
    if (v.quad)
        emit(destination.S4(), first.S4(), second.S4());
    else {
        // Only the two actual D lanes can contribute exception flags.
        emit(destination.toD().S2(), first.toD().S2(), second.toD().S2());
        vectors_.write_d(v.destination, destination);
    }
}
}
