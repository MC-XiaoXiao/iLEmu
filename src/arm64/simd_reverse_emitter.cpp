/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_emitter.hpp"

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
void SimdEmitter::reverse(const arm::VectorReverseOperands& v)
{
    const auto destination = v.quad ? vectors_.write(v.destination) : Q0;
    const auto input = source(Q1, v.source, v.quad);
    if (v.block_bits == 16)
        code_.REV16(destination.B16(), input.B16());
    else if (v.block_bits == 32) {
        if (v.element_bits == 8)
            code_.REV32(destination.B16(), input.B16());
        else
            code_.REV32(destination.H8(), input.H8());
    } else if (v.element_bits == 8)
        code_.REV64(destination.B16(), input.B16());
    else if (v.element_bits == 16)
        code_.REV64(destination.H8(), input.H8());
    else
        code_.REV64(destination.S4(), input.S4());
    if (!v.quad)
        vectors_.write_d(v.destination, destination);
}
}
