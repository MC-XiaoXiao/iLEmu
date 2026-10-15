/* SPDX-License-Identifier: MPL-2.0 */
#include "multiple_emitter.hpp"
#include "compiler.hpp"
#include <array>
#include <bit>
#include <cstddef>

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
std::optional<AddressCache::Key> MultipleEmitter::address(const arm::Instruction& inst)
{
    // PC values and base-load abort rules retain their original guards.
    if (inst.rn == 15 || inst.registers == 0 ||
        (inst.registers & 0x8000U) != 0 ||
        (inst.load && (inst.registers & (1U << inst.rn)) != 0))
        return {};
    const auto length = 4U * static_cast<unsigned>(std::popcount(inst.registers));
    const auto offset = (inst.add ? 0 : -static_cast<int>(length)) +
                        (inst.index == inst.add ? 4 : 0);
    return AddressCache::Key { inst.rn, length,
        static_cast<unsigned>(offset < 0 ? -offset : offset), offset >= 0,
        inst.load, 4, 4 };
}
void MultipleEmitter::emit(const arm::Instruction& inst, std::uint32_t pc,
    std::uint64_t cost, MemoryExits& exits)
{
    const auto key = address(inst);
    if (key) {
        if (const auto pointer = addresses_.find(*key)) {
            access(inst, *pointer, exits);
            return;
        }
    }
    if (key)
        addresses_.begin_guard(*key, inst.condition);
    constexpr auto offset = offsetof(NativeOutcome, multiple);
    constexpr auto memory = offsetof(NativeOutcome, memory);
    const auto base = WReg { static_cast<int>(inst.rn) };
    const auto length =
        4U * static_cast<unsigned>(std::popcount(inst.registers));
    if (inst.add) {
        code_.ADD(W16, base, length);
        code_.MOV(W15, base);
    } else {
        code_.SUB(W16, base, length);
        code_.MOV(W15, W16);
    }
    if (inst.index == inst.add)
        code_.ADD(W15, W15, 4);
    if (!inst.load && (inst.registers & 0x8000U) != 0) {
        code_.LDR(W17, X20, memory + offsetof(DirectMemory, pc_store_offset));
        code_.EOR(W21, W17, 8);
        Label valid { labels_ };
        code_.CBZ(W21, valid);
        code_.EOR(W21, W17, 12);
        code_.CBNZ(W21, exits.get(MemoryExits::Kind::Unsupported));
        code_.l(valid);
        code_.MOV(W21, pc);
        code_.ADD(W17, W21, W17);
        code_.STR(W17, X20,
            offset + offsetof(arm_memory::MultipleTransfer, pc_value));
    }
    Label checked { labels_ }, done { labels_ };
    const auto table = inst.load ? X25 : X26;
    // The fast path only touches aligned ordinary memory in one resident page.
    // A whole-instruction checked boundary handles crossing, MMIO, COW and SMC.
    code_.AND(W17, W15, 3);
    code_.CBNZ(W17, checked);
    code_.CBZ(table, checked);
    code_.AND(W17, W15, 4095);
    code_.ADD(W17, W17, length - 1U);
    code_.LSR(W17, W17, 12);
    code_.CBNZ(W17, checked);
    code_.LSR(W17, W15, 12);
    code_.LDR(X21, table, W17, IndexExt::UXTW, 3);
    code_.CBZ(X21, checked);
    // Loading the base itself uses checked semantics for abort restart rules.
    if (inst.load && (inst.registers & (1U << inst.rn)) != 0)
        code_.B(checked);
    code_.ADD(X21, X21, X15);
    AddressCache::Reference pointer { X21 };
    if (key)
        pointer = addresses_.remember(*key, X21, inst.condition);
    access(inst, pointer, exits);
    code_.B(done);
    code_.l(checked);
    // No destination load has occurred at any guard that reaches this label.
    code_.STR(base, X20, offset + offsetof(arm_memory::MultipleTransfer, base));
    code_.STR(
        W15, X20, offset + offsetof(arm_memory::MultipleTransfer, address));
    code_.STR(W16, X20,
        offset + offsetof(arm_memory::MultipleTransfer, updated_base));
    code_.MOV(W17, arm_memory::multiple_control(inst));
    code_.STR(
        W17, X20, offset + offsetof(arm_memory::MultipleTransfer, control));
    code_.MOV(X21, cost);
    code_.STR(X21, X20, offset + offsetof(arm_memory::MultipleTransfer, ticks));
    code_.B(exits.get(MemoryExits::Kind::Checked));
    code_.l(done);
}
void MultipleEmitter::access(const arm::Instruction& inst,
    AddressCache::Reference address, MemoryExits& exits)
{
    constexpr auto offset = offsetof(NativeOutcome, multiple);
    const auto length = 4U * static_cast<unsigned>(std::popcount(inst.registers));
    const bool folded = address.offset % 4 == 0 && address.offset >= -256 &&
                        address.offset + static_cast<int>(length) - 4 <= 252;
    const auto pointer = folded ? address.base : address.immediate(code_);
    const auto displacement = folded ? address.offset : 0;
    const auto scalar = [&](WReg value, int location) {
        if (inst.load) {
            if (location >= 0)
                code_.LDR(value, pointer, location);
            else
                code_.LDUR(value, pointer, location);
        } else if (location >= 0)
            code_.STR(value, pointer, location);
        else
            code_.STUR(value, pointer, location);
    };
    std::array<unsigned, 16> registers { };
    unsigned count = 0;
    for (unsigned reg = 0; reg < 16; ++reg)
        if ((inst.registers & (1U << reg)) != 0)
            registers[count++] = reg;
    for (unsigned i = 0; i < count;) {
        const auto value = [](unsigned reg) {
            return WReg { reg == 15 ? 17 : static_cast<int>(reg) };
        };
        const bool pair = i + 1U < count;
        const auto first = value(registers[i]);
        const auto second = pair ? value(registers[i + 1U]) : WZR;
        const auto location = displacement + static_cast<int>(i * 4U);
        if (inst.load) {
            if (pair)
                code_.LDP(first, second, pointer, location);
            else
                scalar(first, location);
            if (big_endian_) {
                code_.REV(first, first);
                if (pair)
                    code_.REV(second, second);
            }
        } else {
            if (registers[i] == 15 || (pair && registers[i + 1U] == 15))
                code_.LDR(W17, X20,
                    offset + offsetof(arm_memory::MultipleTransfer, pc_value));
            if (big_endian_) {
                // The guest address is dead after every guard. Keep W16 for
                // writeback and leave all address-cache registers intact.
                code_.REV(W15, first);
                if (pair) {
                    code_.REV(W17, second);
                    code_.STP(W15, W17, pointer, location);
                } else
                    scalar(W15, location);
            } else if (pair)
                code_.STP(first, second, pointer, location);
            else
                scalar(first, location);
        }
        i += pair ? 2U : 1U;
    }
    const bool load_pc = inst.load && (inst.registers & 0x8000U) != 0;
    if (load_pc) {
        Label valid { labels_ };
        code_.TBNZ(W17, 0, valid);
        code_.TBNZ(W17, 1, exits.get(MemoryExits::Kind::Unsupported));
        code_.l(valid);
    }
    addresses_.writeback(inst);
    if (load_pc) {
        code_.BFI(W22, W17, 5, 1);
        code_.AND(W15, W17, 0xfffffffeU);
        code_.B(exits.get(MemoryExits::Kind::Branch));
    }
}
}
