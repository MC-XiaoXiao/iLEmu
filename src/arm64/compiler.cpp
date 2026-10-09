/* SPDX-License-Identifier: MPL-2.0 */
#include "compiler.hpp"
#include "arm/a32_decode.hpp"
#include "arm/t32_decode.hpp"
#include "integer_emitter.hpp"
#include "memory_emitter.hpp"
#include "multiple_emitter.hpp"
#include <algorithm>
#include <cstddef>
#include <deque>
#include <limits>
#include <stdexcept>
#include <oaknut/oaknut.hpp>
#include <type_traits>

namespace ilemu::execution::arm64 {
namespace {
    using namespace oaknut;
    using namespace oaknut::util;
    static_assert(std::is_standard_layout_v<CpuThreadState>);
    static_assert(std::is_standard_layout_v<NativeOutcome>);
    constexpr auto cpsr_offset = offsetof(CpuThreadState, cpsr);
    constexpr unsigned maximum_trace_instructions = 256;
    constexpr unsigned maximum_cycle_copies = 32;

    struct Exit {
        Label label;
        // Empty only when the control callback already published the reason.
        std::optional<StopReason> reason;
        std::uint64_t ticks;
        std::uint32_t state_pc, report_pc, word, svc;
        bool dynamic_pc, instruction;
        unsigned it;
    };
    class Emitter {
    public:
        Emitter(CompiledTrace& trace, bool big_endian, bool thumb)
            : trace_(trace)
            , code_(trace.words, reinterpret_cast<std::uint32_t*>(0x1000))
            , integer_(code_)
            , memory_(code_, integer_, big_endian)
            , thumb_(thumb)
        {
        }
        Label& exit(std::optional<StopReason> reason, std::uint64_t ticks,
            std::uint32_t pc, std::optional<std::uint32_t> word = { },
            std::uint32_t svc = 0, bool dynamic_pc = false,
            std::optional<unsigned> it = { })
        {
            exits_.push_back({ { }, reason, ticks,
                reason == StopReason::Svc ? pc + size_ : pc, pc,
                word.value_or(0), svc, dynamic_pc, word.has_value(),
                it.value_or(it_) });
            return exits_.back().label;
        }
        void location(unsigned it, unsigned size)
        {
            it_ = it;
            size_ = size;
        }
        void retired(unsigned it) { it_ = it; }
        void prologue()
        {
            code_.STP(X19, X20, SP, PreIndexed { }, -160);
            code_.STP(X21, X22, SP, 16);
            code_.STP(X23, X24, SP, 32);
            code_.STP(X25, X26, SP, 48);
            code_.STP(X27, X28, SP, 64);
            code_.STP(X29, X30, SP, 80);
            code_.MOV(X19, X0);
            code_.MOV(X20, X1);
            code_.LDP(X25, X26, X20, offsetof(NativeOutcome, memory));
            code_.LDR(X23, X20, offsetof(NativeOutcome, groups));
            code_.MOV(X24, 0);
            // LR is saved in the frame. Between control calls X30 counts
            // groups until the next poll; BLR overwrites it only at a poll.
            code_.MOV(X30, 1);
            code_.LDR(W22, X19, cpsr_offset);
            code_.MSR(SystemReg::NZCV, X22);
            for (int i = 0; i < 14; i += 2)
                code_.LDP(WReg { i }, WReg { i + 1 }, X19, i * 4);
            code_.LDR(W14, X19, 56);
            code_.align(16);
            code_.l(head_);
        }
        void finish(std::uint32_t pc)
        {
            if (trace_.closed) {
                if (AddSubImm::is_valid(trace_.maximum_ticks))
                    code_.ADD(X24, X24, AddSubImm { trace_.maximum_ticks });
                else {
                    code_.MOV(X16, trace_.maximum_ticks);
                    code_.ADD(X24, X24, X16);
                }
                code_.SUB(X23, X23, 1);
                code_.CBZ(X23, exit(StopReason::None, 0, pc));
                code_.SUB(X30, X30, 1);
                code_.CBNZ(X30, head_);
                // Preserve caller-saved guest registers and flags across the
                // noexcept control call. Callee-saved page caches stay live.
                code_.MRS(X21, SystemReg::NZCV);
                for (int i = 0; i < 14; i += 2)
                    code_.STP(WReg { i }, WReg { i + 1 }, SP, 96 + i * 4);
                code_.STR(W14, SP, 152);
                code_.LDR(X16, X20, offsetof(NativeOutcome, poll));
                code_.LDR(X0, X20, offsetof(NativeOutcome, control));
                code_.BLR(X16);
                code_.STR(W0, X20, offsetof(NativeOutcome, reason));
                code_.MOV(W17, W0);
                code_.MOV(X30, std::max(1U, 4096U / trace_.instructions));
                for (int i = 0; i < 14; i += 2)
                    code_.LDP(WReg { i }, WReg { i + 1 }, SP, 96 + i * 4);
                code_.LDR(W14, SP, 152);
                code_.MSR(SystemReg::NZCV, X21);
                code_.CBNZ(W17, exit({ }, 0, pc));
                code_.B(head_);
            }
            code_.B(exit(StopReason::None,
                trace_.closed ? 0 : trace_.maximum_ticks, pc));
            for (auto& out : exits_) {
                code_.l(out.label);
                code_.MOV(X16, out.ticks);
                code_.ADD(X16, X24, X16);
                code_.STR(X16, X20, offsetof(NativeOutcome, ticks));
                if (!out.dynamic_pc)
                    code_.MOV(W15, out.state_pc);
                code_.STR(W15, X19, 60);
                if (out.dynamic_pc)
                    code_.STR(W15, X20, offsetof(NativeOutcome, pc));
                else {
                    code_.MOV(W16, out.report_pc);
                    code_.STR(W16, X20, offsetof(NativeOutcome, pc));
                }
                if (out.reason) {
                    code_.MOV(W16, static_cast<std::uint32_t>(*out.reason));
                    code_.STR(W16, X20, offsetof(NativeOutcome, reason));
                }
                code_.MOV(W16, out.word);
                code_.STR(W16, X20, offsetof(NativeOutcome, word));
                code_.MOV(W16, out.svc);
                code_.STR(W16, X20, offsetof(NativeOutcome, svc));
                code_.MOV(W16, out.instruction ? 1U : 0U);
                code_.STR(W16, X20, offsetof(NativeOutcome, has_instruction));
                // IT state is deterministic along the emitted path. Publish
                // it at exits instead of adding work to every guest operation.
                if (thumb_) {
                    code_.BFI(W22, WZR, 10, 6);
                    code_.BFI(W22, WZR, 25, 2);
                    if (out.it != 0) {
                        code_.MOV(W16, arm::with_it_state(0, out.it));
                        code_.ORR(W22, W22, W16);
                    }
                }
                code_.B(epilogue_);
            }
            code_.l(epilogue_);
            for (int i = 0; i < 14; i += 2)
                code_.STP(WReg { i }, WReg { i + 1 }, X19, i * 4);
            code_.STR(W14, X19, 56);
            code_.MRS(X21, SystemReg::NZCV);
            code_.AND(W22, W22, 0x0fffffffU);
            code_.ORR(W22, W22, W21);
            code_.STR(W22, X19, cpsr_offset);
            code_.LDP(X21, X22, SP, 16);
            code_.LDP(X23, X24, SP, 32);
            code_.LDP(X25, X26, SP, 48);
            code_.LDP(X27, X28, SP, 64);
            code_.LDP(X29, X30, SP, 80);
            code_.LDP(X19, X20, SP, PostIndexed { }, 160);
            code_.RET();
        }
        void conditional_skip(unsigned condition, Label& target)
        {
            if (condition != 14)
                code_.B(static_cast<Cond>(condition ^ 1U), target);
        }
        // Target legality precedes timing/retirement, matching the interpreter.
        void exchange_target(const arm::Instruction& inst, std::uint32_t pc,
            std::uint32_t word, std::uint64_t ticks)
        {
            code_.MOV(W17,
                integer_.reg(inst.rm,
                    (pc + inst.pc_offset) & (inst.align_pc ? ~3U : ~0U), W17));
            Label valid;
            code_.TBNZ(W17, 0, valid);
            code_.TBZ(W17, 1, valid);
            code_.B(exit(StopReason::UnsupportedInstruction, ticks, pc, word));
            code_.l(valid);
        }
        void exchange(
            const arm::Instruction& inst, std::uint32_t pc, std::uint64_t ticks)
        {
            if (inst.link)
                code_.MOV(W14, (pc + inst.size) | (thumb_ ? 1U : 0U));
            code_.BFI(W22, W17, 5, 1);
            Label arm_target, done;
            code_.TBZ(W17, 0, arm_target);
            code_.AND(W15, W17, 0xfffffffeU);
            code_.B(done);
            code_.l(arm_target);
            code_.AND(W15, W17, 0xfffffffcU);
            code_.l(done);
            code_.B(exit(StopReason::None, ticks, 0, { }, 0, true));
        }
        void invalidate_written_registers(const arm::Instruction& inst)
        {
            if ((inst.kind == arm::InstructionKind::DataProcessing &&
                    (inst.opcode < 8 || inst.opcode > 11)) ||
                inst.kind == arm::InstructionKind::Multiply ||
                inst.kind == arm::InstructionKind::PackHalfword ||
                inst.kind == arm::InstructionKind::WideImmediate ||
                (inst.kind == arm::InstructionKind::Transfer && inst.load))
                memory_.invalidate_register(inst.rd);
            if (inst.kind == arm::InstructionKind::Transfer && inst.writeback)
                memory_.invalidate_register(inst.rn);
            if (inst.kind == arm::InstructionKind::MultipleTransfer)
                memory_.invalidate_all();
            if ((inst.kind == arm::InstructionKind::Branch ||
                    inst.kind == arm::InstructionKind::BranchExchange) &&
                inst.link)
                memory_.invalidate_register(14);
        }
        VectorCodeGenerator& code() { return code_; }
        IntegerEmitter& integer() { return integer_; }
        void transfer(const arm::Instruction& inst, std::uint32_t pc,
            std::uint32_t word, std::uint64_t before, std::uint64_t cost,
            unsigned before_it)
        {
            memory_.emit(inst, pc, cost,
                exit(static_cast<StopReason>(memory_exit), before, pc, word, 0,
                    false, before_it),
                exit(StopReason::UnsupportedInstruction, before, pc, word, 0,
                    false, before_it),
                exit(StopReason::None, before + cost, 0, { }, 0, true));
        }

        void multiple(const arm::Instruction& inst, std::uint32_t pc,
            std::uint32_t word, std::uint64_t before, std::uint64_t cost,
            unsigned before_it, bool big_endian)
        {
            memory_.invalidate_all();
            MultipleEmitter { code_, big_endian }.emit(inst, pc, cost,
                exit(static_cast<StopReason>(multiple_exit), before, pc, word,
                    0, false, before_it),
                exit(StopReason::UnsupportedInstruction, before, pc, word, 0,
                    false, before_it),
                exit(StopReason::None, before + cost, 0, { }, 0, true));
        }

    private:
        CompiledTrace& trace_;
        VectorCodeGenerator code_;
        IntegerEmitter integer_;
        MemoryEmitter memory_;
        bool thumb_;
        unsigned it_ = 0, size_ = 4;
        Label head_, epilogue_;
        std::deque<Exit> exits_;
    };
}

CompiledTrace compile(InstructionSource& source, std::uint32_t pc,
    bool single_step, std::uint64_t maximum_ticks, bool big_endian, bool thumb,
    unsigned it)
{
    const auto region = source.instruction_region(pc);
    if (region.begin > pc || region.end <= pc ||
        region.end > (std::uint64_t { 1 } << 32U))
        throw std::invalid_argument("invalid instruction region");
    CompiledTrace out;
    Emitter emit(out, big_endian, thumb);
    emit.prologue();
    const auto start = pc;
    const auto start_it = it;
    unsigned cycles = 0, cycle_length = 0;
    const auto limit = single_step ? 1U : maximum_trace_instructions;
    for (unsigned n = 0; n < limit; ++n) {
        if (pc < region.begin || pc >= region.end)
            break;
        emit.location(it, thumb ? 2U : 4U);
        std::optional<std::uint32_t> word;
        arm::Instruction inst;
        if (thumb) {
            const auto first = source.fetch16(pc);
            std::optional<std::uint16_t> second;
            if (first && arm::thumb_is_wide(*first)) {
                // A straddling instruction must enter through the dispatcher,
                // which prepares both actual halfwords before native execution.
                if (n != 0 && std::uint64_t { pc } + 4U > region.end)
                    break;
                second = source.fetch16(pc + 2U);
            }
            if (first && (!arm::thumb_is_wide(*first) || second)) {
                word = *first | (std::uint32_t { second.value_or(0) } << 16U);
                inst = arm::decode_t32(*first, second, it);
            }
        } else {
            word = source.fetch32(pc);
            if (word)
                inst = arm::decode_a32(*word);
        }
        if (n == 0)
            out.first_instruction = word;
        if (!word) {
            emit.code().B(
                emit.exit(StopReason::FetchFault, out.maximum_ticks, pc));
            break;
        }
        if (n != 0 && std::uint64_t { pc } + inst.size > region.end)
            break;
        const bool straddles = std::uint64_t { pc } + inst.size > region.end;
        emit.location(it, inst.size);
        const auto before = out.maximum_ticks;
        const bool unsupported =
            inst.kind == arm::InstructionKind::Unsupported ||
            ((inst.kind == arm::InstructionKind::Transfer ||
                 inst.kind == arm::InstructionKind::MultipleTransfer) &&
                !source.data_memory());
        if (inst.condition == 15 || (unsupported && inst.condition == 14)) {
            emit.code().B(emit.exit(
                StopReason::UnsupportedInstruction, before, pc, word));
            break;
        }
        if (unsupported)
            emit.code().B(static_cast<Cond>(inst.condition),
                emit.exit(
                    StopReason::UnsupportedInstruction, before, pc, word));
        Label exchange_failed;
        if (inst.kind == arm::InstructionKind::BranchExchange) {
            emit.conditional_skip(inst.condition, exchange_failed);
            emit.exchange_target(inst, pc, *word, before);
        }
        const auto cost = source.ticks_for_instruction(pc, *word);
        const bool valid_timing = cost != 0 && cost <= maximum_ticks - before;
        if (!valid_timing) {
            if (inst.kind == arm::InstructionKind::BranchExchange &&
                inst.condition != 14)
                emit.code().l(exchange_failed);
            emit.code().B(
                emit.exit(StopReason::InvalidTiming, before, pc, word));
            break;
        }
        if (!unsupported &&
            (inst.kind == arm::InstructionKind::Transfer ||
                inst.kind == arm::InstructionKind::MultipleTransfer))
            out.accesses_memory = true;
        out.maximum_ticks += cost;
        ++out.instructions;
        if (inst.kind == arm::InstructionKind::Breakpoint) {
            emit.code().B(emit.exit(StopReason::Breakpoint, out.maximum_ticks,
                pc, word, 0, false, it));
            break;
        }
        const auto next_it = thumb ? (inst.kind == arm::InstructionKind::IfThen
                                             ? inst.immediate
                                             : arm::advance_it(it))
                                   : 0U;
        emit.retired(next_it);
        const auto next_pc = pc + inst.size;
        if (inst.kind == arm::InstructionKind::BranchExchange) {
            emit.exchange(inst, pc, out.maximum_ticks);
            if (inst.condition == 14)
                break;
            emit.code().l(exchange_failed);
            pc = next_pc;
        } else if (inst.kind == arm::InstructionKind::Branch) {
            if (inst.condition != 14)
                emit.conditional_skip(inst.condition,
                    emit.exit(StopReason::None, out.maximum_ticks, next_pc));
            if (inst.link)
                emit.code().MOV(W14, next_pc | (thumb ? 1U : 0U));
            pc = ((pc + inst.pc_offset) & (inst.align_pc ? ~3U : ~0U)) +
                 inst.immediate;
            if (inst.exchange) {
                emit.code().EOR(W22, W22, 0x20U);
                emit.code().B(
                    emit.exit(StopReason::None, out.maximum_ticks, pc));
                break;
            }
        } else if (inst.kind == arm::InstructionKind::CompareBranch) {
            auto& fallthrough =
                emit.exit(StopReason::None, out.maximum_ticks, next_pc);
            if (inst.opcode == 0)
                emit.code().CBNZ(
                    WReg { static_cast<int>(inst.rn) }, fallthrough);
            else
                emit.code().CBZ(
                    WReg { static_cast<int>(inst.rn) }, fallthrough);
            pc += inst.pc_offset + inst.immediate;
        } else if (inst.kind == arm::InstructionKind::Svc) {
            if (inst.condition == 14) {
                emit.code().B(emit.exit(StopReason::Svc, out.maximum_ticks, pc,
                    word, inst.immediate));
                break;
            }
            emit.code().B(static_cast<Cond>(inst.condition),
                emit.exit(StopReason::Svc, out.maximum_ticks, pc, word,
                    inst.immediate));
            pc = next_pc;
        } else {
            Label skip;
            const bool writes_pc =
                inst.kind == arm::InstructionKind::DataProcessing &&
                inst.rd == 15 && (inst.opcode < 8 || inst.opcode > 11);
            if (!unsupported) {
                emit.conditional_skip(inst.condition, skip);
                switch (inst.kind) {
                case arm::InstructionKind::Multiply:
                    emit.integer().multiply(inst);
                    break;
                case arm::InstructionKind::MultipleTransfer:
                    emit.multiple(
                        inst, pc, *word, before, cost, it, big_endian);
                    break;
                case arm::InstructionKind::Transfer:
                    emit.transfer(inst, pc, *word, before, cost, it);
                    break;
                case arm::InstructionKind::WideImmediate:
                    emit.integer().wide_immediate(inst);
                    break;
                case arm::InstructionKind::PackHalfword:
                    emit.integer().pack_halfword(inst);
                    break;
                case arm::InstructionKind::DataProcessing:
                    emit.integer().alu(inst, pc);
                    if (writes_pc) {
                        emit.code().AND(W15, W15, ~1U);
                        emit.code().B(emit.exit(StopReason::None,
                            out.maximum_ticks, 0, { }, 0, true));
                    }
                    break;
                default:
                    break;
                }
                if (inst.condition != 14)
                    emit.code().l(skip);
            }
            pc = next_pc;
            if (writes_pc && inst.condition == 14)
                break;
        }
        emit.invalidate_written_registers(inst);
        it = next_it;
        if (straddles)
            break;
        if (!single_step && pc == start && it == start_it) {
            ++cycles;
            if (cycle_length == 0)
                cycle_length = n + 1;
            const auto copies =
                std::min(maximum_cycle_copies, limit / cycle_length);
            if (cycles >= copies) {
                out.closed = true;
                break;
            }
        }
    }
    emit.finish(pc);
    return out;
}
}
