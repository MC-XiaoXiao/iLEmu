/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_emitter.hpp"

namespace ilemu::execution::arm64 {
void SimdEmitter::conversion(const arm::VectorFloatingConversionOperands& v)
{
    using namespace oaknut::util;
    const auto input = source(Q1, v.source, v.quad);
    const auto destination = v.quad ? vectors_.write(v.destination) : Q0;
    const auto emit = [&](auto d, auto s) {
        if (v.to_integer) {
            if (v.unsigned_value) code_.FCVTZU(d, s);
            else code_.FCVTZS(d, s);
        } else {
            if (v.unsigned_value) code_.UCVTF(d, s);
            else code_.SCVTF(d, s);
        }
    };
    if (v.quad)
        emit(destination.S4(), input.S4());
    else {
        emit(destination.toD().S2(), input.toD().S2());
        vectors_.write_d(v.destination, destination);
    }
}
}
