/* SPDX-License-Identifier: MPL-2.0 */
#include "vector_memory_emitter.hpp"
#include "compiler.hpp"
#include <cstddef>

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
void VectorMemoryEmitter::access(const arm::Instruction& inst)
{
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    constexpr auto vectors = offsetof(CpuThreadState, extension_registers);
    if (v.mode == arm::VectorTransferMode::Multiple) {
        for (unsigned r = 0; r < v.count; ++r) {
            const auto state_offset = vectors + (v.first + r) * 8U;
            code_.LDR(
                D0, inst.load ? X21 : X19, inst.load ? r * 8U : state_offset);
            if (big_endian_) {
                if (v.element_size == 2)
                    code_.REV16(V0.B8(), V0.B8());
                else if (v.element_size == 4)
                    code_.REV32(V0.B8(), V0.B8());
                else if (v.element_size == 8)
                    code_.REV64(V0.B8(), V0.B8());
            }
            code_.STR(
                D0, inst.load ? X19 : X21, inst.load ? state_offset : r * 8U);
        }
        return;
    }
    if (!inst.load) {
        code_.LDR(D0, X19, vectors + v.first * 8U);
        if (v.element_size == 1)
            code_.UMOV(W17, Q0.Belem()[v.lane]);
        else if (v.element_size == 2)
            code_.UMOV(W17, Q0.Helem()[v.lane]);
        else
            code_.UMOV(W17, Q0.Selem()[v.lane]);
    } else {
        if (v.element_size == 1)
            code_.LDRB(W17, X21);
        else if (v.element_size == 2)
            code_.LDRH(W17, X21);
        else
            code_.LDR(W17, X21);
    }
    if (big_endian_) {
        if (v.element_size == 2)
            code_.REV16(W17, W17);
        else if (v.element_size == 4)
            code_.REV(W17, W17);
    }
    if (!inst.load) {
        if (v.element_size == 1)
            code_.STRB(W17, X21);
        else if (v.element_size == 2)
            code_.STRH(W17, X21);
        else
            code_.STR(W17, X21);
    } else {
        if (v.mode == arm::VectorTransferMode::Replicate) {
            if (v.element_size == 1)
                code_.DUP(V0.B8(), W17);
            else if (v.element_size == 2)
                code_.DUP(V0.H4(), W17);
            else
                code_.DUP(V0.S2(), W17);
        } else {
            code_.LDR(D0, X19, vectors + v.first * 8U);
            if (v.element_size == 1)
                code_.INS(Q0.Belem()[v.lane], W17);
            else if (v.element_size == 2)
                code_.INS(Q0.Helem()[v.lane], W17);
            else
                code_.INS(Q0.Selem()[v.lane], W17);
        }
        for (unsigned r = 0; r < v.count; ++r)
            code_.STR(D0, X19, vectors + (v.first + r) * 8U);
    }
}
void VectorMemoryEmitter::emit(
    const arm::Instruction& inst, std::uint64_t cost, Label& checked_exit)
{
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    const auto length = v.mode == arm::VectorTransferMode::Multiple
                            ? v.count * 8U
                            : v.element_size;
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
    access(inst);
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
