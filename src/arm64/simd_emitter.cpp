/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_emitter.hpp"
#include "execution/arm_state.hpp"
#include <cstddef>

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
void SimdEmitter::duplicate(const arm::VectorDuplicateOperands& v)
{
    constexpr auto vectors = offsetof(CpuThreadState, extension_registers);
    // V0/V1 are caller-saved scratch. Guest D/Q values stay in architectural
    // storage, so poll callbacks and checked exits need no vector spill frame.
    if (v.core_source) {
        const WReg source { static_cast<int>(v.source) };
        if (v.element_bits == 8)
            code_.DUP(V0.B16(), source);
        else if (v.element_bits == 16)
            code_.DUP(V0.H8(), source);
        else
            code_.DUP(V0.S4(), source);
    } else {
        code_.LDR(D1, X19, vectors + v.source * 8U);
        if (v.element_bits == 8)
            code_.DUP(V0.B16(), Q1.Belem()[v.lane]);
        else if (v.element_bits == 16)
            code_.DUP(V0.H8(), Q1.Helem()[v.lane]);
        else
            code_.DUP(V0.S4(), Q1.Selem()[v.lane]);
    }
    if (v.quad)
        code_.STR(Q0, X19, vectors + v.destination * 8U);
    else
        code_.STR(D0, X19, vectors + v.destination * 8U);
}
}
