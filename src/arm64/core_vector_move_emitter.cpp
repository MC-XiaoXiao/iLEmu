/* SPDX-License-Identifier: MPL-2.0 */
#include "simd_emitter.hpp"

namespace ilemu::execution::arm64 {
void SimdEmitter::core_move(const arm::VectorCoreMoveOperands& v)
{
    using namespace oaknut;
    for (unsigned i = 0; i < (v.pair ? 2U : 1U); ++i) {
        const auto word = v.first_word + i;
        const auto value = v.to_core ? vectors_.read(word / 2U)
                                    : vectors_.write(word / 2U);
        const WReg core { static_cast<int>(i == 0 ? v.core : v.core2) };
        const auto bit = (word % 4U) * 32U + v.bit_offset;
        if (!v.to_core) {
            if (v.element_bits == 8)
                code_.INS(value.Belem()[bit / 8U], core);
            else if (v.element_bits == 16)
                code_.INS(value.Helem()[bit / 16U], core);
            else
                code_.INS(value.Selem()[bit / 32U], core);
        } else if (v.element_bits == 8) {
            if (v.sign_extend)
                code_.SMOV(core, value.Belem()[bit / 8U]);
            else
                code_.UMOV(core, value.Belem()[bit / 8U]);
        } else if (v.element_bits == 16) {
            if (v.sign_extend)
                code_.SMOV(core, value.Helem()[bit / 16U]);
            else
                code_.UMOV(core, value.Helem()[bit / 16U]);
        } else
            code_.UMOV(core, value.Selem()[bit / 32U]);
    }
}
}
