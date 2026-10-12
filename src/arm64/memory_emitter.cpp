/* SPDX-License-Identifier: MPL-2.0 */
#include "memory_emitter.hpp"
#include "compiler.hpp"
#include <cstddef>

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
namespace {
    constexpr auto memory_offset = offsetof(NativeOutcome, memory);
    constexpr auto transfer_offset = offsetof(NativeOutcome, transfer);
    template<typename... Address>
    void scalar_read(VectorCodeGenerator& code, const arm::Instruction& inst,
        bool big_endian, Address... address)
    {
        const auto value =
            inst.rd == 15 ? W17 : WReg { static_cast<int>(inst.rd) };
        // Byte loads are endian-independent; little-endian halfwords can
        // also fold extension into the host memory instruction.
        const bool signed_load = inst.sign_extend &&
                                 (!big_endian || inst.access_size == 1);
        if (inst.access_size == 1) {
            if (signed_load)
                code.LDRSB(value, address...);
            else
                code.LDRB(value, address...);
        } else if (inst.access_size == 2) {
            if (signed_load)
                code.LDRSH(value, address...);
            else
                code.LDRH(value, address...);
        } else
            code.LDR(value, address...);
        if (big_endian && inst.access_size != 1) {
            if (inst.access_size == 2)
                code.REV16(value, value);
            else
                code.REV(value, value);
        }
        if (inst.sign_extend && !signed_load)
            code.SXTH(value, value);
    }
}
void MemoryEmitter::store_value(const arm::Instruction& inst, std::uint32_t pc)
{
    if (inst.rd == 15) {
        code_.LDR(
            W17, X20, memory_offset + offsetof(DirectMemory, pc_store_offset));
        Label eight, done;
        code_.TBZ(W17, 2, eight);
        code_.MOV(W17, pc + 12U);
        code_.B(done);
        code_.l(eight);
        code_.MOV(W17, pc + 8U);
        code_.l(done);
    } else
        code_.MOV(W17, WReg { static_cast<int>(inst.rd) });
}
void MemoryEmitter::endian(unsigned size, WReg value)
{
    if (!big_endian_ || size == 1)
        return;
    if (size == 2)
        code_.REV16(value, value);
    else
        code_.REV(value, value);
}
std::optional<AddressCache::Key> MemoryEmitter::address(
    const arm::Instruction& inst, std::uint32_t pc) const
{
    if (!inst.index || inst.writeback ||
        (!inst.load && inst.rd == 15))
        return { };
    // Load-PC requires word alignment even when ordinary reads permit
    // unaligned data. PC stores retain their separate PC-value policy guard.
    const bool load_pc = inst.load && inst.rd == 15 &&
                         inst.kind != arm::InstructionKind::TableBranch;
    const bool dual = inst.kind == arm::InstructionKind::DualTransfer;
    AddressCache::Key key { inst.rn, dual ? 8U : inst.access_size,
        inst.immediate_operand ? inst.immediate : 0U, inst.add, inst.load,
        load_pc || dual ? 4U : 1U, inst.access_size };
    if (inst.rn == 15)
        key.pc_base = (pc + inst.pc_offset) & (inst.align_pc ? ~3U : ~0U);
    if (!inst.immediate_operand) {
        // RRX reads the incoming carry, which can change within a trace.
        // Register-controlled shifts would add a third address dependency.
        if (inst.register_shift || inst.rm == 15 ||
            (inst.shift == arm::ShiftKind::Ror && inst.shift_amount == 0))
            return { };
        key.index = inst.rm;
        key.shift = inst.shift;
        key.shift_amount = inst.shift_amount;
    }
    return key;
}
void MemoryEmitter::access(const arm::Instruction& inst, std::uint32_t pc,
    XReg pointer, Exits& exits)
{
    if (inst.kind == arm::InstructionKind::DualTransfer) {
        const WReg first { static_cast<int>(inst.rd) };
        const WReg second { static_cast<int>(inst.rs) };
        if (inst.load) {
            code_.LDP(first, second, pointer);
            endian(4, first);
            endian(4, second);
        } else if (big_endian_) {
            code_.REV(W15, first);
            code_.REV(W17, second);
            code_.STP(W15, W17, pointer);
        } else
            code_.STP(first, second, pointer);
        if (inst.writeback)
            code_.MOV(WReg { static_cast<int>(inst.rn) }, W16);
        return;
    }
    if (inst.load) {
        scalar_read(code_, inst, big_endian_, pointer);
        if (inst.kind == arm::InstructionKind::TableBranch) {
            code_.MOV(W15, pc + 4U);
            code_.ADD(W15, W15, W17, AddSubShift::LSL, 1);
            code_.B(exits.get(ExitKind::Branch));
            return;
        }
        if (inst.rd == 15) {
            Label valid;
            code_.TBNZ(W17, 0, valid);
            code_.TBNZ(W17, 1, exits.get(ExitKind::Unsupported));
            code_.l(valid);
        }
    } else {
        WReg value = W17;
        if (inst.rd == 15 || (big_endian_ && inst.access_size != 1)) {
            store_value(inst, pc);
            endian(inst.access_size, W17);
        } else
            value = WReg { static_cast<int>(inst.rd) };
        if (inst.access_size == 1)
            code_.STRB(value, pointer);
        else if (inst.access_size == 2)
            code_.STRH(value, pointer);
        else
            code_.STR(value, pointer);
    }
    if (inst.writeback)
        code_.MOV(WReg { static_cast<int>(inst.rn) }, W16);
    if (inst.load) {
        if (inst.rd == 15) {
            code_.BFI(W22, W17, 5, 1);
            code_.AND(W15, W17, 0xfffffffeU);
            code_.B(exits.get(ExitKind::Branch));
        }
    }
}
void MemoryEmitter::emit(const arm::Instruction& inst, std::uint32_t pc,
    std::uint64_t cost, Exits& exits)
{
    const auto key = address(inst, pc);
    if (key) {
        if (const auto pointer = addresses_.find(*key)) {
            access(inst, pc, *pointer, exits);
            return;
        }
    }
    if (fast_reads_ && key && inst.condition == 14 && inst.load &&
        inst.rd != 15 && inst.kind == arm::InstructionKind::Transfer &&
        (key->inputs() & (1U << inst.rd)) != 0) {
        dependent_read(inst, pc, cost, exits);
        return;
    }
    Label checked, done;
    const bool dual = inst.kind == arm::InstructionKind::DualTransfer;
    const auto length = dual ? 8U : inst.access_size;
    if (key && inst.condition == 14)
        addresses_.begin_guard(*key);
    // W16 holds the offset/new base; W15 the effective guest address.
    integer_.address_offset(inst, pc);
    const auto base = integer_.reg(
        inst.rn, (pc + inst.pc_offset) & (inst.align_pc ? ~3U : ~0U), W15);
    if (inst.add)
        code_.ADD(W16, base, W16);
    else
        code_.SUB(W16, base, W16);
    code_.MOV(W15, inst.index ? W16 : base);
    if (inst.rd == 15 && inst.load &&
        inst.kind != arm::InstructionKind::TableBranch) {
        code_.AND(W17, W15, 3);
        code_.CBNZ(W17, exits.get(ExitKind::Unsupported));
    }
    if (inst.rd == 15 && !inst.load) {
        code_.LDR(
            W21, X20, memory_offset + offsetof(DirectMemory, pc_store_offset));
        code_.EOR(W17, W21, 8);
        Label valid;
        code_.CBZ(W17, valid);
        code_.EOR(W17, W21, 12);
        code_.CBNZ(W17, exits.get(ExitKind::Unsupported));
        code_.l(valid);
    }
    const auto table = inst.load ? X25 : X26;
    if (dual) {
        code_.AND(W17, W15, 3);
        code_.CBNZ(W17, checked);
    }
    code_.CBZ(table, checked);
    if (inst.access_size != 1) {
        Label unaligned_allowed;
        code_.LDR(W21, X20,
            memory_offset + offsetof(DirectMemory, permits_unaligned));
        code_.CBNZ(W21, unaligned_allowed);
        code_.AND(W17, W15, inst.access_size - 1U);
        code_.CBNZ(W17, checked);
        code_.l(unaligned_allowed);
        // Cross-page and wraparound accesses require one checked transaction.
        code_.AND(W17, W15, 4095);
        code_.ADD(W17, W17, length - 1U);
        code_.LSR(W17, W17, 12);
        code_.CBNZ(W17, checked);
    }
    code_.LSR(W17, W15, 12);
    code_.LDR(X21, table, W17, IndexExt::UXTW, 3);
    code_.CBZ(X21, checked);
    code_.ADD(X21, X21, X15);
    XReg pointer = X21;
    if (key && inst.condition == 14)
        pointer = addresses_.remember(*key, pointer);
    access(inst, pc, pointer, exits);
    code_.B(done);
    code_.l(checked);
    code_.STR(
        W15, X20, transfer_offset + offsetof(arm_memory::Transfer, address));
    code_.STR(W16, X20,
        transfer_offset + offsetof(arm_memory::Transfer, updated_base));
    if (!inst.load)
        store_value(inst, pc);
    else
        code_.MOV(W17, 0);
    code_.STR(
        W17, X20, transfer_offset + offsetof(arm_memory::Transfer, value));
    if (dual)
        code_.STR(WReg { static_cast<int>(inst.rs) }, X20,
            transfer_offset + offsetof(arm_memory::Transfer, second_value));
    code_.MOV(W17, arm_memory::control(inst));
    code_.STR(
        W17, X20, transfer_offset + offsetof(arm_memory::Transfer, control));
    code_.MOV(X21, cost);
    code_.STR(
        X21, X20, transfer_offset + offsetof(arm_memory::Transfer, ticks));
    code_.B(exits.get(ExitKind::Checked));
    code_.l(done);
}
void MemoryEmitter::dependent_read(const arm::Instruction& inst,
    std::uint32_t pc, std::uint64_t cost, Exits& exits)
{
    auto address = W15;
    if (inst.immediate_operand && inst.immediate == 0 && inst.rn != 15) {
        address = WReg { static_cast<int>(inst.rn) };
    } else {
        integer_.address_offset(inst, pc);
        const auto base = integer_.reg(inst.rn,
            (pc + inst.pc_offset) & (inst.align_pc ? ~3U : ~0U), W15);
        if (inst.add)
            code_.ADD(W16, base, W16);
        else
            code_.SUB(W16, base, W16);
        code_.MOV(W15, W16);
    }
    pending_.push_back({ inst, cost, address, { },
        &exits.get(ExitKind::Checked) });
    auto& pending = pending_.back();
    // Aligned scalars cannot cross a page. Keep ordinary unaligned reads
    // inline too; only a checked transaction leaves the native body.
    if (inst.access_size != 1) {
        Label aligned;
        code_.AND(W17, address, inst.access_size - 1U);
        code_.CBZ(W17, aligned);
        code_.LDR(W17, X20,
            memory_offset + offsetof(DirectMemory, permits_unaligned));
        code_.CBZ(W17, pending.entry);
        code_.AND(W17, address, 4095);
        code_.ADD(W17, W17, inst.access_size - 1U);
        code_.TBNZ(W17, 12, pending.entry);
        code_.l(aligned);
    }
    // The view stays invariant under the invocation's instruction lease.
    // A missing view exits at the first dominating unconditional read.
    if (!read_table_guarded_) {
        code_.CBZ(X25, pending.entry);
        read_table_guarded_ = true;
    }
    code_.LSR(W17, address, 12);
    code_.LDR(X21, X25, W17, IndexExt::UXTW, 3);
    code_.CBZ(X21, pending.entry);
    // Fold address addition into the load without occupying invariant slots.
    scalar_read(code_, inst, big_endian_, X21, address, IndexExt::UXTW);
}
void MemoryEmitter::finish()
{
    for (auto& pending : pending_) {
        code_.l(pending.entry);
        code_.STR(pending.address, X20,
            transfer_offset + offsetof(arm_memory::Transfer, address));
        code_.STR(pending.address, X20,
            transfer_offset + offsetof(arm_memory::Transfer, updated_base));
        code_.MOV(W17, 0);
        code_.STR(W17, X20,
            transfer_offset + offsetof(arm_memory::Transfer, value));
        code_.MOV(W17, arm_memory::control(pending.instruction));
        code_.STR(W17, X20,
            transfer_offset + offsetof(arm_memory::Transfer, control));
        code_.MOV(X21, pending.cost);
        code_.STR(X21, X20,
            transfer_offset + offsetof(arm_memory::Transfer, ticks));
        code_.B(*pending.checked);
    }
}

}
