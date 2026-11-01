/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_emitter.hpp"

namespace ilemu::execution::arm64 {
void SimdEmitter::shift(const arm::VectorShiftImmediateOperands& v)
{
    using namespace oaknut::util;
    const auto input = source(Q1, v.source, v.quad);
    const auto destination = v.quad ? vectors_.write(v.destination) : Q0;
    if (!v.quad && (v.accumulate || v.insert))
        vectors_.read_d(destination, v.destination);
    const auto emit = [&](auto d, auto s) {
        if (!v.right) {
            if (v.insert) code_.SLI(d, s, v.amount);
            else code_.SHL(d, s, v.amount);
        } else if (v.insert) code_.SRI(d, s, v.amount);
        else if (v.accumulate) {
            if (v.unsigned_value) {
                if (v.rounding) code_.URSRA(d, s, v.amount);
                else code_.USRA(d, s, v.amount);
            } else {
                if (v.rounding) code_.SRSRA(d, s, v.amount);
                else code_.SSRA(d, s, v.amount);
            }
        } else if (v.unsigned_value) {
            if (v.rounding) code_.URSHR(d, s, v.amount);
            else code_.USHR(d, s, v.amount);
        } else {
            if (v.rounding) code_.SRSHR(d, s, v.amount);
            else code_.SSHR(d, s, v.amount);
        }
    };
    switch (v.element_bits) {
    case 8: emit(destination.B16(), input.B16()); break;
    case 16: emit(destination.H8(), input.H8()); break;
    case 32: emit(destination.S4(), input.S4()); break;
    case 64: emit(destination.D2(), input.D2()); break;
    }
    if (!v.quad)
        vectors_.write_d(v.destination, destination);
}
}
