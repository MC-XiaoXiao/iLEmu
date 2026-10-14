/* SPDX-License-Identifier: MPL-2.0 */
#include "vector_memory_emitter.hpp"

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
void VectorMemoryEmitter::scalar_multiple(const arm::Instruction& inst, XReg pointer)
{
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    const auto first = v.first * (v.element_size / 4U);
    const auto words = v.count * (v.element_size / 4U);
    for (unsigned offset = 0; offset < words;) {
        const auto word = first + offset;
        const bool quad = word % 4U == 0 && words - offset >= 4;
        const bool pair = word % 2U == 0 && words - offset >= 2;
        const auto count = quad ? 4U : pair ? 2U : 1U;
        auto value = quad ? (inst.load ? vectors_.write(word / 2U)
                                     : vectors_.read(word / 2U)) : Q0;
        // Zero-offset lane transfers preserve neighbours without a scratch copy.
        if (!quad && !big_endian_ && offset == 0) {
            const auto bank = inst.load ? vectors_.write(word / 2U)
                                       : vectors_.read(word / 2U);
            if (pair)
                access_lane(inst.load, bank.Delem()[word / 2U % 2U], pointer);
            else
                access_lane(inst.load, bank.Selem()[word % 4U], pointer);
            offset += count;
            continue;
        }
        if (inst.load) {
            if (quad)
                code_.LDUR(value, pointer, offset * 4U);
            else if (pair)
                code_.LDUR(value.toD(), pointer, offset * 4U);
            else
                code_.LDUR(value.toS(), pointer, offset * 4U);
            if (big_endian_)
                endian(value, v.element_size, quad);
            if (!quad) {
                if (pair)
                    vectors_.write_d(word / 2U, value);
                else
                    vectors_.write_s(word, value);
            }
        } else {
            if (!quad && !big_endian_) {
                if (pair)
                    code_.STUR(vectors_.scalar_d(word / 2U, Q0), pointer,
                        offset * 4U);
                else
                    code_.STUR(vectors_.scalar_s(word, Q0), pointer,
                        offset * 4U);
                offset += count;
                continue;
            }
            if (pair && !quad)
                vectors_.read_d(value, word / 2U);
            else if (!pair)
                code_.DUP(value.S4(), vectors_.read(word / 2U).Selem()[word % 4U]);
            if (big_endian_) {
                if (quad)
                    code_.MOV(Q0.B16(), value.B16());
                value = Q0;
                endian(value, v.element_size, quad);
            }
            if (quad)
                code_.STUR(value, pointer, offset * 4U);
            else if (pair)
                code_.STUR(value.toD(), pointer, offset * 4U);
            else
                code_.STUR(value.toS(), pointer, offset * 4U);
        }
        offset += count;
    }
}
}
