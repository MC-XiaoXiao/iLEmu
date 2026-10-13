/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_emitter.hpp"

namespace ilemu::execution::arm64 {
void SimdEmitter::floating(const arm::VectorFloatingArithmeticOperands& v)
{
    using namespace oaknut::util;
    const auto first = source(Q1, v.first, v.quad);
    const auto second = source(Q2, v.second, v.quad);
    const auto destination = v.quad ? vectors_.write(v.destination) : Q0;
    const auto emit = [&](auto d, auto a, auto b) {
        using Op = arm::FloatingArithmeticOperation;
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
