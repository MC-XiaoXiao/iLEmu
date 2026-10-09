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
}
void MemoryEmitter::store_value(
    const arm::Instruction& inst, std::uint32_t pc)
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
void MemoryEmitter::invalidate_register(unsigned reg)
{
    for (auto& entry : addresses_)
        if (entry && entry->base == reg)
            entry.reset();
}
std::optional<MemoryEmitter::Address> MemoryEmitter::address(
    const arm::Instruction& inst) const
{
    if (!inst.immediate_operand || !inst.index || inst.writeback ||
        inst.rn == 15 || inst.rd == 15)
        return { };
    return Address { inst.rn, inst.access_size, inst.immediate, inst.add,
        inst.load };
}
void MemoryEmitter::access(const arm::Instruction& inst, std::uint32_t pc,
    XReg pointer, Label& unsupported_exit, Label& branch_exit)
{
    if (inst.load) {
        const auto value =
            inst.rd == 15 ? W17 : WReg { static_cast<int>(inst.rd) };
        if (inst.access_size == 1)
            code_.LDRB(value, pointer);
        else if (inst.access_size == 2)
            code_.LDRH(value, pointer);
        else
            code_.LDR(value, pointer);
        endian(inst.access_size, value);
        if (inst.sign_extend) {
            if (inst.access_size == 1)
                code_.SXTB(value, value);
            else
                code_.SXTH(value, value);
        }
        if (inst.rd == 15) {
            Label valid;
            code_.TBNZ(W17, 0, valid);
            code_.TBNZ(W17, 1, unsupported_exit);
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
            code_.B(branch_exit);
        }
    }
}
void MemoryEmitter::emit(const arm::Instruction& inst, std::uint32_t pc,
    std::uint64_t cost, Label& checked_exit, Label& unsupported_exit,
    Label& branch_exit)
{
    const auto key = address(inst);
    if (key) {
        for (unsigned slot = 0; slot < addresses_.size(); ++slot) {
            if (addresses_[slot] == key) {
                access(inst, pc, XReg { 27 + static_cast<int>(slot) },
                    unsupported_exit, branch_exit);
                return;
            }
        }
    }
    Label checked, done;
    // W16 holds the offset/new base; W15 the effective guest address.
    integer_.address_offset(inst, pc);
    const auto base = integer_.reg(inst.rn, pc, W15);
    if (inst.add)
        code_.ADD(W16, base, W16);
    else
        code_.SUB(W16, base, W16);
    code_.MOV(W15, inst.index ? W16 : base);
    if (inst.rd == 15 && inst.load) {
        code_.AND(W17, W15, 3);
        code_.CBNZ(W17, unsupported_exit);
    }
    if (inst.rd == 15 && !inst.load) {
        code_.LDR(
            W21, X20, memory_offset + offsetof(DirectMemory, pc_store_offset));
        code_.EOR(W17, W21, 8);
        Label valid;
        code_.CBZ(W17, valid);
        code_.EOR(W17, W21, 12);
        code_.CBNZ(W17, unsupported_exit);
        code_.l(valid);
    }
    const auto table = inst.load ? X25 : X26;
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
        code_.ADD(W17, W17, inst.access_size - 1U);
        code_.LSR(W17, W17, 12);
        code_.CBNZ(W17, checked);
    }
    code_.LSR(W17, W15, 12);
    code_.LDR(X21, table, W17, IndexExt::UXTW, 3);
    code_.CBZ(X21, checked);
    code_.ADD(X21, X21, X15);
    XReg pointer = X21;
    if (key && inst.condition == 14) {
        const auto slot = next_address_++ % addresses_.size();
        addresses_[slot] = key;
        pointer = XReg { 27 + static_cast<int>(slot) };
        code_.MOV(pointer, X21);
    }
    access(inst, pc, pointer, unsupported_exit, branch_exit);
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
    code_.MOV(W17, arm_memory::control(inst));
    code_.STR(
        W17, X20, transfer_offset + offsetof(arm_memory::Transfer, control));
    code_.MOV(X21, cost);
    code_.STR(
        X21, X20, transfer_offset + offsetof(arm_memory::Transfer, ticks));
    code_.B(checked_exit);
    code_.l(done);
}
}
