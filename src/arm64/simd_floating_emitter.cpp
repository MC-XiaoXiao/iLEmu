/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_emitter.hpp"

namespace ilemu::execution::arm64 {
void SimdEmitter::floating(const arm::VectorFloatingArithmeticOperands& v)
{
    using namespace oaknut::util;
    // Unreserved D values retain their packed view. A resident D is independent
    // of its neighbour and needs no extraction or scratch copy.
    const auto operand = [&](oaknut::QReg scratch, unsigned d) {
        if (!v.quad)
            if (auto resident = vectors_.resident_d(d))
                return oaknut::QReg { resident->index() };
        return v.quad || d % 2U == 0 ? vectors_.read(d)
                                    : source(scratch, d, false);
    };
    const auto first = operand(Q1, v.first);
    const auto second = v.scalar_lane ? Q2 : operand(Q2, v.second);
    if (v.scalar_lane) {
        const auto resident = vectors_.resident_d(v.second);
        const auto bank = resident ? oaknut::QReg { resident->index() }
                                   : vectors_.read(v.second);
        const auto lane = *v.scalar_lane + (resident ? 0U : (v.second % 2U) * 2U);
        code_.DUP(Q2.S4(), bank.Selem()[lane]);
    }
    const auto resident = v.quad ? std::optional<oaknut::DReg> {}
                                 : vectors_.resident_d(v.destination);
    const auto destination = v.quad ? vectors_.write(v.destination)
        : resident ? oaknut::QReg { resident->index() } : Q0;
    using Op = arm::FloatingArithmeticOperation;
    if (v.pairwise) {
        code_.FADDP(destination.toD().S2(), first.toD().S2(), second.toD().S2());
        vectors_.write_d(v.destination, destination);
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
            if (!resident)
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
            accumulate(destination.toD().S2(), product.toD().S2());
            vectors_.write_d(v.destination, destination);
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
