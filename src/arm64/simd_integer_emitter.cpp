/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_emitter.hpp"
namespace ilemu::execution::arm64 {
void SimdEmitter::integer(const arm::VectorIntegerOperands& v)
{
    using namespace oaknut::util;
    const auto first = source(Q1, v.first, v.quad);
    const auto second = source(Q2, v.second, v.quad);
    const auto destination = v.quad ? vectors_.write(v.destination) : Q0;
    using Op = arm::VectorIntegerOperation;
    if (!v.quad && (v.operation == Op::MultiplyAdd ||
                      v.operation == Op::MultiplySubtract))
        vectors_.read_d(destination, v.destination);
    const auto emit = [&](auto d, auto a, auto b) {
        switch (v.operation) {
        case Op::Add: code_.ADD(d, a, b); break;
        case Op::Subtract: code_.SUB(d, a, b); break;
        case Op::Multiply: code_.MUL(d, a, b); break;
        case Op::MultiplyAdd: code_.MLA(d, a, b); break;
        case Op::MultiplySubtract: code_.MLS(d, a, b); break;
        }
    };
    switch (v.element_bits) {
    case 8: emit(destination.B16(), first.B16(), second.B16()); break;
    case 16: emit(destination.H8(), first.H8(), second.H8()); break;
    case 32: emit(destination.S4(), first.S4(), second.S4()); break;
    case 64:
        // Only add/sub have 64-bit elements in these A32/T32 families.
        if (v.operation == Op::Add)
            code_.ADD(destination.D2(), first.D2(), second.D2());
        else
            code_.SUB(destination.D2(), first.D2(), second.D2());
        break;
    }
    if (!v.quad)
        vectors_.write_d(v.destination, destination);
}
}
