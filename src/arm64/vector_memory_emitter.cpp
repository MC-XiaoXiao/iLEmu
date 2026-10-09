/* SPDX-License-Identifier: MPL-2.0 */
#include "vector_memory_emitter.hpp"
#include "compiler.hpp"
#include <cstddef>

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
namespace {
    void endian(VectorCodeGenerator& code, QReg value, unsigned size, bool quad)
    {
        if (quad) {
            if (size == 2)
                code.REV16(value.B16(), value.B16());
            else if (size == 4)
                code.REV32(value.B16(), value.B16());
            else if (size == 8)
                code.REV64(value.B16(), value.B16());
        } else {
            if (size == 2)
                code.REV16(value.toD().B8(), value.toD().B8());
            else if (size == 4)
                code.REV32(value.toD().B8(), value.toD().B8());
            else if (size == 8)
                code.REV64(value.toD().B8(), value.toD().B8());
        }
    }
}
void VectorMemoryEmitter::access(const arm::Instruction& inst, XReg pointer)
{
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    if (v.mode == arm::VectorTransferMode::Multiple) {
        for (unsigned r = 0; r < v.count;) {
            const auto d = v.first + r;
            const bool quad = d % 2U == 0 && r + 1U < v.count;
            if (inst.load) {
                const auto value = quad ? vectors_.write(d) : Q0;
                // An odd first D can place a Q pair at an eight-byte offset.
                // Unscaled offsets represent that without another address add.
                if (quad)
                    code_.LDUR(value, pointer, r * 8U);
                else
                    code_.LDR(value.toD(), pointer, r * 8U);
                if (big_endian_)
                    endian(code_, value, v.element_size, quad);
                if (!quad)
                    vectors_.write_d(d, value);
            } else {
                auto value = quad ? vectors_.read(d) : Q0;
                if (!quad)
                    vectors_.read_d(value, d);
                if (big_endian_) {
                    if (quad)
                        code_.MOV(Q0.B16(), value.B16());
                    value = Q0;
                    endian(code_, value, v.element_size, quad);
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
void VectorMemoryEmitter::emit(
    const arm::Instruction& inst, std::uint64_t cost, Label& checked_exit)
{
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    const auto length = v.mode == arm::VectorTransferMode::Multiple
                            ? v.count * 8U
                            : v.element_size;
    const AddressCache::Key key { inst.rn, length, 0, true, inst.load,
        v.alignment, v.element_size };
    if (!inst.writeback) {
        if (const auto pointer = addresses_.find(key)) {
            access(inst, *pointer);
            return;
        }
    }
    constexpr auto memory = offsetof(NativeOutcome, memory);
    constexpr auto transfer = offsetof(NativeOutcome, vector);
    Label checked, aligned, done;
    code_.MOV(W15, WReg { static_cast<int>(inst.rn) });
    if (inst.rm == 13 || inst.rm == 15)
        code_.ADD(W16, W15, length);
    else
        code_.ADD(W16, W15, WReg { static_cast<int>(inst.rm) });
    // W21 carries the effective alignment to the checked transaction.
    code_.MOV(W21, v.alignment);
    if (v.element_size > v.alignment) {
        code_.LDR(W17, X20, memory + offsetof(DirectMemory, permits_unaligned));
        code_.CBNZ(W17, aligned);
        code_.MOV(W21, v.element_size);
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
    auto pointer = X21;
    if (!inst.writeback && inst.condition == 14)
        pointer = addresses_.remember(key, pointer);
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
