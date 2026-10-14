/* SPDX-License-Identifier: MPL-2.0 */
#include "memory_emitter.hpp"
#include "compiler.hpp"
#include <cstddef>
#include <bit>

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
namespace {
    constexpr auto memory_offset = offsetof(NativeOutcome, memory);
    constexpr auto transfer_offset = offsetof(NativeOutcome, transfer);
    template<bool Unscaled = false, typename... Address>
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
            if (signed_load) {
                if constexpr (Unscaled)
                    code.LDURSB(value, address...);
                else
                    code.LDRSB(value, address...);
            } else {
                if constexpr (Unscaled)
                    code.LDURB(value, address...);
                else
                    code.LDRB(value, address...);
            }
        } else if (inst.access_size == 2) {
            if (signed_load) {
                if constexpr (Unscaled)
                    code.LDURSH(value, address...);
                else
                    code.LDRSH(value, address...);
            } else {
                if constexpr (Unscaled)
                    code.LDURH(value, address...);
                else
                    code.LDRH(value, address...);
            }
        } else {
            if constexpr (Unscaled)
                code.LDUR(value, address...);
            else
                code.LDR(value, address...);
        }
        if (big_endian && inst.access_size != 1) {
            if (inst.access_size == 2)
                code.REV16(value, value);
            else
                code.REV(value, value);
        }
        if (inst.sign_extend && !signed_load)
            code.SXTH(value, value);
    }
    template<bool Unscaled, typename... Address>
    void scalar_write(VectorCodeGenerator& code, unsigned size, WReg value,
        Address... address)
    {
        if (size == 1) {
            if constexpr (Unscaled)
                code.STURB(value, address...);
            else
                code.STRB(value, address...);
        } else if (size == 2) {
            if constexpr (Unscaled)
                code.STURH(value, address...);
            else
                code.STRH(value, address...);
        } else {
            if constexpr (Unscaled)
                code.STUR(value, address...);
            else
                code.STR(value, address...);
        }
    }
}
void MemoryEmitter::store_value(const arm::Instruction& inst, std::uint32_t pc)
{
    if (inst.rd == 15) {
        code_.LDR(
            W17, X20, memory_offset + offsetof(DirectMemory, pc_store_offset));
        Label eight { label_memory() }, done { label_memory() };
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
std::pmr::memory_resource& MemoryEmitter::label_memory()
{
    if (!labels_)
        labels_.emplace(std::pmr::pool_options {
            .max_blocks_per_chunk = 8, .largest_required_pool_block = 256 },
            pending_.get_allocator().resource());
    return *labels_;
}
std::optional<AddressCache::Key> MemoryEmitter::address(
    const arm::Instruction& inst, std::uint32_t pc)
{
    if ((!inst.index && !inst.writeback) ||
        (inst.writeback && !AddressDisplacements::writeback(inst)) ||
        (!inst.load && inst.rd == 15))
        return { };
    // Load-PC requires word alignment even when ordinary reads permit
    // unaligned data. PC stores retain their separate PC-value policy guard.
    const bool load_pc = inst.load && inst.rd == 15 &&
                         inst.kind != arm::InstructionKind::TableBranch;
    const bool dual = inst.kind == arm::InstructionKind::DualTransfer;
    AddressCache::Key key { inst.rn, dual ? 8U : inst.access_size,
        inst.immediate_operand && inst.index ? inst.immediate : 0U, inst.add, inst.load,
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
bool MemoryEmitter::is_dependent_read(
    const arm::Instruction& inst, const AddressCache::Key& key)
{
    return inst.condition == 14 && inst.load && inst.rd != 15 &&
        inst.kind == arm::InstructionKind::Transfer &&
        (key.inputs() & (1U << inst.rd)) != 0;
}
std::optional<unsigned> MemoryEmitter::dependent_read_destination(
    const arm::Instruction& inst, std::uint32_t pc)
{
    const auto key = address(inst, pc);
    if (key && is_dependent_read(inst, *key))
        return inst.rd;
    return { };
}
void MemoryEmitter::access(const arm::Instruction& inst, std::uint32_t pc,
    AddressCache::Reference address, Exits& exits)
{
    const auto resolve = [&] {
        if (!address.indexed)
            return address.immediate(code_);
        integer_.address_offset(inst, pc);
        if (inst.add)
            code_.ADD(X21, address.base, W16, AddSubExt::UXTW);
        else
            code_.SUB(X21, address.base, W16, AddSubExt::UXTW);
        return X21;
    };
    if (inst.kind == arm::InstructionKind::DualTransfer) {
        const WReg first { static_cast<int>(inst.rd) };
        const WReg second { static_cast<int>(inst.rs) };
        const auto pair = [&](XReg base, int offset) {
            if (inst.load) {
                code_.LDP(first, second, base, offset);
                endian(4, first);
                endian(4, second);
            } else if (big_endian_) {
                code_.REV(W15, first);
                code_.REV(W17, second);
                code_.STP(W15, W17, base, offset);
            } else
                code_.STP(first, second, base, offset);
        };
        if (!address.indexed && address.offset % 4 == 0 &&
            address.offset >= -256 && address.offset <= 252)
            pair(address.base, address.offset);
        else
            pair(resolve(), 0);
        addresses_.writeback(inst);
        return;
    }
    const auto transfer = [&]<bool Unscaled>(auto... location) {
        if (inst.load)
            scalar_read<Unscaled>(code_, inst, big_endian_, location...);
        else {
            WReg value = W17;
            if (inst.rd == 15 || (big_endian_ && inst.access_size != 1)) {
                store_value(inst, pc);
                endian(inst.access_size, W17);
            } else
                value = WReg { static_cast<int>(inst.rd) };
            scalar_write<Unscaled>(code_, inst.access_size, value, location...);
        }
    };
    const auto scale = static_cast<unsigned>(std::countr_zero(inst.access_size));
    if (address.indexed && inst.shift == arm::ShiftKind::Lsl && inst.add &&
        (inst.shift_amount == 0 || inst.shift_amount == scale))
        transfer.template operator()<false>(address.base,
            WReg { static_cast<int>(inst.rm) }, IndexExt::UXTW, inst.shift_amount);
    else if (address.indexed && inst.shift == arm::ShiftKind::Lsl && !inst.add) {
        code_.NEG(W16, WReg { static_cast<int>(inst.rm) },
            AddSubShift::LSL, inst.shift_amount);
        transfer.template operator()<false>(address.base, W16, IndexExt::SXTW);
    } else if (address.indexed)
        transfer.template operator()<false>(resolve());
    else if (address.offset >= 0 &&
        static_cast<unsigned>(address.offset) % inst.access_size == 0)
        transfer.template operator()<false>(address.base, address.offset);
    else if (address.offset >= -256 && address.offset <= 255)
        transfer.template operator()<true>(address.base, address.offset);
    else
        transfer.template operator()<false>(resolve());
    if (inst.load) {
        if (inst.kind == arm::InstructionKind::TableBranch) {
            code_.MOV(W15, pc + 4U);
            code_.ADD(W15, W15, W17, AddSubShift::LSL, 1);
            code_.B(exits.get(ExitKind::Branch));
            return;
        }
        if (inst.rd == 15) {
            Label valid { label_memory() };
            code_.TBNZ(W17, 0, valid);
            code_.TBNZ(W17, 1, exits.get(ExitKind::Unsupported));
            code_.l(valid);
        }
    }
    addresses_.writeback(inst);
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
    if (fast_reads_ && key && is_dependent_read(inst, *key)) {
        dependent_read(inst, pc, cost, exits);
        return;
    }
    auto& labels = label_memory();
    Label checked { labels }, done { labels };
    const bool dual = inst.kind == arm::InstructionKind::DualTransfer;
    const auto length = dual ? 8U : inst.access_size;
    if (key)
        addresses_.begin_guard(*key, inst.condition);
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
        Label valid { labels };
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
        Label unaligned_allowed { labels };
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
    AddressCache::Reference pointer { X21 };
    if (key)
        pointer = addresses_.remember(*key, X21, inst.condition);
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
        Label aligned { label_memory() };
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
    read_pages_.resolve(W17, pending.entry);
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
