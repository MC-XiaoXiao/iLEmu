/* SPDX-License-Identifier: MPL-2.0 */
#include "vector_memory_emitter.hpp"

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
bool VectorMemoryEmitter::resident_scalar(
    const arm::Instruction& inst, AddressCache::Reference address)
{
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    if (v.mode == arm::VectorTransferMode::Lane &&
        v.count == 1 && v.element_size == 4) {
        if (auto resident = vectors_.resident_d(v.first)) {
            const auto lane = QReg { resident->index() }.Selem()[v.lane];
            const auto pointer = address.immediate(code_);
            if (!big_endian_)
                access_lane(inst.load, lane, pointer);
            else {
                if (inst.load)
                    code_.LDR(W17, pointer);
                else
                    code_.UMOV(W17, lane);
                code_.REV(W17, W17);
                if (inst.load)
                    code_.INS(lane, W17);
                else
                    code_.STR(W17, pointer);
            }
            return true;
        }
    }
    if (v.mode != arm::VectorTransferMode::ScalarMultiple || v.count != 1)
        return false;
    const auto access = [&](auto value) {
        const auto offset = address.offset;
        if (offset >= 0 && static_cast<unsigned>(offset) % v.element_size == 0 &&
            static_cast<unsigned>(offset) / v.element_size < 4096) {
            if (inst.load)
                code_.LDR(value, address.base, offset);
            else
                code_.STR(value, address.base, offset);
        } else if (offset >= -256 && offset <= 255) {
            if (inst.load)
                code_.LDUR(value, address.base, offset);
            else
                code_.STUR(value, address.base, offset);
        } else {
            const auto pointer = address.immediate(code_);
            if (inst.load)
                code_.LDR(value, pointer);
            else
                code_.STR(value, pointer);
        }
    };
    const auto direct = [&](auto reg) {
        const decltype(reg) value { inst.load || !big_endian_ ? reg.index() : 0 };
        if (!inst.load && big_endian_) {
            code_.FMOV(value, reg);
            endian(Q0, v.element_size, false);
        }
        access(value);
        if (inst.load && big_endian_)
            endian(QReg { reg.index() }, v.element_size, false);
    };
    if (v.element_size == 8) {
        if (auto resident = vectors_.resident_d(v.first)) {
            direct(*resident);
            return true;
        }
        if (auto pair = vectors_.resident_pair(v.first)) {
            if (!inst.load && big_endian_) {
                vectors_.read_d(Q0, v.first);
                endian(Q0, 8, false);
                access(D0);
                return true;
            }
            const auto offset = address.offset;
            const bool folded = offset % 4 == 0 && offset >= -256 && offset <= 252;
            const auto pointer = folded ? address.base : address.immediate(code_);
            const auto displacement = folded ? offset : 0;
            auto low = pair->first, high = pair->second;
            if (big_endian_)
                std::swap(low, high);
            if (inst.load)
                code_.LDP(low, high, pointer, displacement);
            else
                code_.STP(low, high, pointer, displacement);
            if (inst.load && big_endian_) {
                endian(QReg { low.index() }, 4, false);
                endian(QReg { high.index() }, 4, false);
            }
            return true;
        }
    } else {
        if (auto resident = vectors_.resident_s(v.first)) {
            direct(*resident);
            return true;
        }
        if (auto wide = vectors_.resident_d(v.first / 2U)) {
            const auto lane = QReg { wide->index() }.Selem()[v.first % 2U];
            if (!big_endian_) {
                access_lane(inst.load, lane, address.immediate(code_));
                return true;
            }
            if (!inst.load)
                code_.UMOV(W17, lane);
            if (!inst.load && big_endian_)
                code_.REV(W17, W17);
            access(W17);
            if (inst.load && big_endian_)
                code_.REV(W17, W17);
            if (inst.load)
                code_.INS(lane, W17);
            return true;
        }
    }
    return false;
}
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
