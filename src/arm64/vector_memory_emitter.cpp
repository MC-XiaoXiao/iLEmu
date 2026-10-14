/* SPDX-License-Identifier: MPL-2.0 */
#include "vector_memory_emitter.hpp"
#include "compiler.hpp"
#include <cstddef>

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
void VectorMemoryEmitter::endian(QReg value, unsigned size, bool quad)
{
    if (quad) {
        if (size == 2)
            code_.REV16(value.B16(), value.B16());
        else if (size == 4)
            code_.REV32(value.B16(), value.B16());
        else if (size == 8)
            code_.REV64(value.B16(), value.B16());
    } else {
        if (size == 2)
            code_.REV16(value.toD().B8(), value.toD().B8());
        else if (size == 4)
            code_.REV32(value.toD().B8(), value.toD().B8());
        else if (size == 8)
            code_.REV64(value.toD().B8(), value.toD().B8());
    }
}
void VectorMemoryEmitter::access(const arm::Instruction& inst, AddressCache::Reference address)
{
    if (resident_scalar(inst, address))
        return;
    const auto pointer = address.immediate(code_);
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    if (v.mode == arm::VectorTransferMode::ScalarMultiple) {
        scalar_multiple(inst, pointer);
        return;
    }
    if (v.mode == arm::VectorTransferMode::Multiple) {
        for (unsigned r = 0; r < v.count;) {
            const auto d = v.first + r;
            const bool quad = d % 2U == 0 && r + 1U < v.count;
            if (!quad && !big_endian_ && r == 0) {
                const auto bank = inst.load ? vectors_.write(d) : vectors_.read(d);
                access_lane(inst.load, bank.Delem()[d % 2U], pointer);
                ++r;
                continue;
            }
            if (inst.load) {
                const auto value = quad ? vectors_.write(d) : Q0;
                // An odd first D can place a Q pair at an eight-byte offset.
                // Unscaled offsets represent that without another address add.
                if (quad)
                    code_.LDUR(value, pointer, r * 8U);
                else
                    code_.LDR(value.toD(), pointer, r * 8U);
                if (big_endian_)
                    endian(value, v.element_size, quad);
                if (!quad)
                    vectors_.write_d(d, value);
            } else {
                if (!quad && !big_endian_) {
                    code_.STR(vectors_.scalar_d(d, Q0), pointer, r * 8U);
                    ++r;
                    continue;
                }
                auto value = quad ? vectors_.read(d) : Q0;
                if (!quad)
                    vectors_.read_d(value, d);
                if (big_endian_) {
                    if (quad)
                        code_.MOV(Q0.B16(), value.B16());
                    value = Q0;
                    endian(value, v.element_size, quad);
                }
                if (quad)
                    code_.STUR(value, pointer, r * 8U);
                else
                    code_.STR(value.toD(), pointer, r * 8U);
            }
            r += quad ? 2U : 1U;
        }
        return;
    }
    const auto lane = v.lane + (v.first % 2U) * (8U / v.element_size);
    if (!big_endian_ && v.mode == arm::VectorTransferMode::Lane &&
        v.element_size == 4) {
        const auto bank = inst.load ? vectors_.write(v.first)
                                    : vectors_.read(v.first);
        access_lane(inst.load, bank.Selem()[lane], pointer);
        return;
    }
    if (!inst.load) {
        const auto value = vectors_.read(v.first);
        if (v.element_size == 1)
            code_.UMOV(W17, value.Belem()[lane]);
        else if (v.element_size == 2)
            code_.UMOV(W17, value.Helem()[lane]);
        else
            code_.UMOV(W17, value.Selem()[lane]);
    } else {
        if (v.element_size == 1)
            code_.LDRB(W17, pointer);
        else if (v.element_size == 2)
            code_.LDRH(W17, pointer);
        else
            code_.LDR(W17, pointer);
    }
    if (big_endian_) {
        if (v.element_size == 2)
            code_.REV16(W17, W17);
        else if (v.element_size == 4)
            code_.REV(W17, W17);
    }
    if (!inst.load) {
        if (v.element_size == 1)
            code_.STRB(W17, pointer);
        else if (v.element_size == 2)
            code_.STRH(W17, pointer);
        else
            code_.STR(W17, pointer);
    } else if (v.mode == arm::VectorTransferMode::Replicate) {
        if (v.element_size == 1)
            code_.DUP(Q0.toD().B8(), W17);
        else if (v.element_size == 2)
            code_.DUP(Q0.toD().H4(), W17);
        else
            code_.DUP(Q0.toD().S2(), W17);
        for (unsigned r = 0; r < v.count; ++r)
            vectors_.write_d(v.first + r, Q0);
    } else {
        const auto value = vectors_.write(v.first);
        if (v.element_size == 1)
            code_.INS(value.Belem()[lane], W17);
        else if (v.element_size == 2)
            code_.INS(value.Helem()[lane], W17);
        else
            code_.INS(value.Selem()[lane], W17);
    }
}
std::optional<AddressCache::Key> VectorMemoryEmitter::address(const arm::Instruction& inst)
{
    if (inst.writeback || inst.rn == 15)
        return {};
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    const bool scalar = v.mode == arm::VectorTransferMode::ScalarMultiple;
    return AddressCache::Key { inst.rn, arm_memory::vector_transfer_length(v),
        scalar && inst.index ? inst.immediate : 0U, inst.add, inst.load,
        v.alignment, scalar ? 4U : v.element_size };
}
void VectorMemoryEmitter::emit(
    const arm::Instruction& inst, std::uint32_t pc, std::uint64_t cost,
    Label& checked_exit)
{
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    const auto length = arm_memory::vector_transfer_length(v);
    const bool scalar = v.mode == arm::VectorTransferMode::ScalarMultiple;
    const auto natural_alignment = scalar ? 4U : v.element_size;
    const auto key = address(inst);
    if (key) {
        if (const auto pointer = addresses_.find(*key)) {
            access(inst, *pointer);
            return;
        }
    }
    constexpr auto memory = offsetof(NativeOutcome, memory);
    constexpr auto transfer = offsetof(NativeOutcome, vector);
    Label checked, aligned, done;
    if (key && inst.condition == 14)
        addresses_.begin_guard(*key);
    if (inst.rn == 15)
        code_.MOV(W15, inst.align_pc ? (pc + inst.pc_offset) & ~3U
                                   : pc + inst.pc_offset);
    else
        code_.MOV(W15, WReg { static_cast<int>(inst.rn) });
    if (scalar) {
        if (inst.add)
            code_.ADD(W16, W15, inst.immediate);
        else
            code_.SUB(W16, W15, inst.immediate);
        if (inst.index)
            code_.MOV(W15, W16);
    } else if (inst.rm == 13 || inst.rm == 15)
        code_.ADD(W16, W15, length);
    else
        code_.ADD(W16, W15, WReg { static_cast<int>(inst.rm) });
    // W21 carries the effective alignment to the checked transaction.
    code_.MOV(W21, v.alignment);
    if (natural_alignment > v.alignment) {
        code_.LDR(W17, X20, memory + offsetof(DirectMemory, permits_unaligned));
        code_.CBNZ(W17, aligned);
        code_.MOV(W21, natural_alignment);
        code_.l(aligned);
    }
    code_.SUB(W17, W21, 1);
    code_.AND(W17, W15, W17);
    code_.CBNZ(W17, checked);
    const auto table = inst.load ? X25 : X26;
    code_.CBZ(table, checked);
    // Entire transfer must fit one page. Checked accesses preserve
    // architectural prefix completion and release executable page locks before
    // any store.
    code_.AND(W17, W15, 4095);
    code_.ADD(W17, W17, length - 1U);
    code_.LSR(W17, W17, 12);
    code_.CBNZ(W17, checked);
    code_.LSR(W17, W15, 12);
    code_.LDR(X17, table, W17, IndexExt::UXTW, 3);
    code_.CBZ(X17, checked);
    code_.ADD(X21, X17, X15);
    AddressCache::Reference pointer { X21 };
    if (key && inst.condition == 14)
        pointer = addresses_.remember(*key, X21);
    access(inst, pointer);
    if (inst.writeback)
        code_.MOV(WReg { static_cast<int>(inst.rn) }, W16);
    code_.B(done);
    code_.l(checked);
    code_.STR(
        W15, X20, transfer + offsetof(arm_memory::VectorTransfer, address));
    code_.STR(W16, X20,
        transfer + offsetof(arm_memory::VectorTransfer, updated_base));
    code_.STR(
        W21, X20, transfer + offsetof(arm_memory::VectorTransfer, alignment));
    code_.MOV(W17, arm_memory::vector_control(inst));
    code_.STR(
        W17, X20, transfer + offsetof(arm_memory::VectorTransfer, control));
    code_.MOV(X17, cost);
    code_.STR(X17, X20, transfer + offsetof(arm_memory::VectorTransfer, ticks));
    code_.B(checked_exit);
    code_.l(done);
}
}
