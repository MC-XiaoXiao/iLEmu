/* SPDX-License-Identifier: MPL-2.0 */
#include "compiler.hpp"
#include "native_frame.hpp"
#include "arm/program_status.hpp"
#include "cycle_tier.hpp"
#include "instruction_window.hpp"
#include "dynamic_backedge.hpp"
#include "exchange_paths.hpp"
#include "arm/t32_decode.hpp"
#include "arm/multiply.hpp"
#include "integer_emitter.hpp"
#include "memory_emitter.hpp"
#include "multiple_emitter.hpp"
#include "simd_emitter.hpp"
#include "floating_emitter.hpp"
#include "floating_environment.hpp"
#include "arm/floating_conversion.hpp"
#include "arm/floating_arithmetic.hpp"
#include "arm/floating_comparison.hpp"
#include "arm/floating_system.hpp"
#include "arm/pc_write.hpp"
#include "system_emitter.hpp"
#include "exclusive_emitter.hpp"
#include "vector_memory_emitter.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <limits>
#include <map>
#include <memory_resource>
#include <stdexcept>
#include <oaknut/oaknut.hpp>
#include <type_traits>
#include <tuple>
#include <utility>

namespace ilemu::execution::arm64 {
namespace {
    using namespace oaknut;
    using namespace oaknut::util;
    static_assert(std::is_standard_layout_v<CpuThreadState>);
    static_assert(std::is_standard_layout_v<NativeOutcome>);
    constexpr auto cpsr_offset = offsetof(CpuThreadState, cpsr);
    constexpr unsigned maximum_trace_instructions = 256;

    struct Exit {
        Label label;
        // Empty only when the control callback already published the reason.
        std::optional<StopReason> reason;
        std::uint64_t ticks;
        std::uint32_t state_pc, report_pc, word, svc;
        bool dynamic_pc, instruction, backedge;
        unsigned it;
        auto payload() const
        {
            return std::tuple { reason, state_pc, report_pc, word, svc,
                dynamic_pc, instruction, it };
        }
    };
    class Emitter {
    public:
        Emitter(CompiledTrace& trace, bool big_endian, bool thumb,
            std::uint32_t pc, unsigned it, bool single_step,
            std::pmr::memory_resource* temporary, bool fast_reads, bool cache_reads)
            : trace_(trace)
            , code_(trace.words, reinterpret_cast<std::uint32_t*>(0x1000))
            , vectors_(code_)
            , integer_(code_)
            , exchanges_(code_, integer_, thumb)
            , memory_(code_, integer_, big_endian, fast_reads, cache_reads, temporary)
            , thumb_(thumb)
            , backedge_(pc, it, thumb, !single_step)
            , exits_(temporary)
        {
        }
        Label& exit(std::optional<StopReason> reason, std::uint64_t ticks,
            std::uint32_t pc, std::optional<std::uint32_t> word = { },
            std::uint32_t svc = 0, bool dynamic_pc = false,
            std::optional<unsigned> it = { }, bool backedge = true)
        {
            exits_.push_back({ { }, reason, ticks,
                reason == StopReason::Svc ? pc + size_ : pc, pc,
                word.value_or(0), svc, dynamic_pc, word.has_value(), backedge,
                it.value_or(it_) });
            return exits_.back().label;
        }
        void location(unsigned it, unsigned size)
        {
            it_ = it;
            size_ = size;
        }
        void retired(unsigned it) { it_ = it; }
        void disable_backedges() { backedge_.disable(); }
        unsigned expansion_copies(unsigned maximum)
        {
            const auto total_growth = memory_.read_expansion_growth();
            if (total_growth == 0)
                return maximum;
            const auto words = trace_.words.size() - cycle_end_;
            const auto growth = total_growth - cycle_growth_;
            cycle_end_ = trace_.words.size();
            cycle_growth_ = total_growth;
            if (first_cycle_words_ == 0) {
                first_cycle_words_ = words;
                first_cycle_growth_ = growth;
                return std::max(1U, static_cast<unsigned>(
                    maximum * (words - growth) / words));
            }
            // First-use invariant guards can dwarf the repeated body. Charge
            // the first copy separately, then measure the steady second copy
            // before multiplying added read code across the old copy budget.
            const auto budget = first_cycle_words_ - first_cycle_growth_ +
                (maximum - 1U) * (words - growth);
            const auto copies = budget > first_cycle_words_
                ? 1U + static_cast<unsigned>((budget - first_cycle_words_) / words)
                : 1U;
            return std::min(maximum, std::max(2U, copies));
        }
        bool continue_cycle(std::uint32_t fallthrough)
        {
            Exit* continuation = nullptr;
            for (auto& out : exits_) {
                if (!accepts_backedge(out))
                    continue;
                if (continuation)
                    return false;
                continuation = &out;
            }
            if (!continuation)
                return false;
            // A failed instruction condition follows its ordinary paid exit.
            code_.B(exit(StopReason::None, trace_.maximum_ticks, fallthrough));
            code_.l(continuation->label);
            // Binding resolved every incoming branch. A new label keeps the
            // precise prefix exit; a matching target falls into the next copy.
            continuation->label = Label { };
            backedge_.branch_on_mismatch(code_, continuation->label);
            continued_cycle_ = true;
            return true;
        }
        void prologue()
        {
            NativeFrame::entry(trace_.words);
            entry_end_ = cycle_end_ = trace_.words.size();
            code_.l(head_);
        }
        void finish(std::uint32_t pc)
        {
            const bool natural_backedge = trace_.closed;
            trace_.closed |= std::any_of(exits_.begin(), exits_.end(),
                [&](const Exit& out) { return accepts_backedge(out); });
            // Only a guarded dynamic edge may loop. An ordinary fallthrough
            // still publishes its paid prefix and next PC to the dispatcher.
            if (!natural_backedge)
                code_.B(exit(StopReason::None, trace_.maximum_ticks, pc));
            if (trace_.closed) {
                code_.l(loop_tail_);
                if (AddSubImm::is_valid(trace_.maximum_ticks))
                    code_.ADD(X24, X24, AddSubImm { trace_.maximum_ticks });
                else {
                    code_.MOV(X16, trace_.maximum_ticks);
                    code_.ADD(X24, X24, X16);
                }
                code_.SUB(X23, X23, 1);
                code_.CBZ(X23, exit(StopReason::None, 0, backedge_.pc(), { },
                    0, false, backedge_.it()));
                code_.SUB(X30, X30, 1);
                code_.CBNZ(X30, head_);
                // Preserve caller-saved guest registers and flags across the
                // noexcept control call. Callee-saved page caches stay live.
                code_.MRS(X21, SystemReg::NZCV);
                for (int i = 0; i < 14; i += 2)
                    code_.STP(WReg { i }, WReg { i + 1 }, SP, 96 + i * 4);
                code_.STR(W14, SP, 152);
                floating_.leave(code_);
                code_.LDR(X16, X20, offsetof(NativeOutcome, poll));
                code_.LDR(X0, X20, offsetof(NativeOutcome, control));
                vectors_.flush();
                code_.BLR(X16);
                floating_.resume(code_);
                vectors_.restore();
                code_.STR(W0, X20, offsetof(NativeOutcome, reason));
                code_.MOV(W17, W0);
                code_.MOV(X30, std::max(1U, 4096U / trace_.instructions));
                for (int i = 0; i < 14; i += 2)
                    code_.LDP(WReg { i }, WReg { i + 1 }, SP, 96 + i * 4);
                code_.LDR(W14, SP, 152);
                code_.MSR(SystemReg::NZCV, X21);
                code_.CBNZ(W17, exit({ }, 0, backedge_.pc(), { }, 0, false,
                    backedge_.it()));
                code_.B(head_);
            }
            if (natural_backedge)
                code_.B(exit(StopReason::None, 0, pc));
            memory_.finish();
            for (auto& fallback : exchanges_.pending()) {
                code_.l(fallback.label);
                location(fallback.it, fallback.instruction.size);
                if (arm::alu_writes_pc(fallback.instruction)) {
                    alu_target(fallback.instruction, fallback.pc,
                        fallback.word, fallback.before, fallback.interworks);
                    retired(fallback.next_it);
                    alu_branch(fallback.instruction, fallback.pc,
                        fallback.ticks, fallback.interworks, false);
                } else {
                    exchange_target(fallback.instruction, fallback.pc,
                        fallback.word, fallback.before);
                    retired(fallback.next_it);
                    // A mismatched target cannot return to the trace entry.
                    exchange(fallback.instruction, fallback.pc,
                        fallback.ticks, false);
                }
            }
            struct SharedExit {
                Label label;
                const Exit* example = nullptr;
                unsigned count = 0;
            };
            std::pmr::map<decltype(Exit { }.payload()), SharedExit> shared {
                exits_.get_allocator().resource() };
            // Natural and continued cycles can repeat payloads through
            // control exits as well as copied guest instructions.
            const bool share = natural_backedge || continued_cycle_;
            if (share) {
                for (const auto& out : exits_) {
                    auto& payload = shared[out.payload()];
                    payload.example = &out;
                    ++payload.count;
                }
            }
            for (auto& out : exits_) {
                code_.l(out.label);
                if (accepts_backedge(out))
                    backedge_.emit(code_, loop_tail_);
                code_.MOV(X16, out.ticks);
                if (!share) {
                    publish_exit(out);
                    continue;
                }
                auto& payload = shared.at(out.payload());
                if (payload.count == 1)
                    publish_exit(out);
                else
                    code_.B(payload.label);
            }
            for (auto& [key, payload] : shared) {
                if (payload.count == 1)
                    continue;
                code_.l(payload.label);
                publish_exit(*payload.example);
            }
            code_.l(epilogue_);
            floating_.leave(code_);
            vectors_.flush();
            NativeFrame::publish(trace_.words);
            memory_.leave();
            NativeFrame::leave(trace_.words);
            std::vector<std::uint32_t> initial;
            VectorCodeGenerator entry(initial);
            memory_.enter(entry);
            floating_.enter(entry);
            vectors_.restore(entry);
            memory_.addresses().finalize(trace_.closed, trace_.words, entry);
            if (!initial.empty()) {
                // Only the entry prefix precedes this boundary and it contains
                // no branches. All body labels/relative branches move together;
                // backedges skip initialization. Preserve head alignment.
                entry.align(16);
                trace_.words.insert(trace_.words.begin() +
                        static_cast<std::ptrdiff_t>(entry_end_),
                    initial.begin(), initial.end());
            }
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
            guard_exchange_target(pc, word, ticks);
        }
        void guard_exchange_target(std::uint32_t pc,
            std::uint32_t word, std::uint64_t ticks)
        {
            Label valid;
            code_.TBNZ(W17, 0, valid);
            code_.TBZ(W17, 1, valid);
            code_.B(exit(StopReason::UnsupportedInstruction, ticks, pc, word));
            code_.l(valid);
        }
        void exchange(
            const arm::Instruction& inst, std::uint32_t pc, std::uint64_t ticks,
            bool backedge = true)
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
            code_.B(exit(StopReason::None, ticks, 0, { }, 0, true,
                { }, backedge));
        }
        void alu_target(const arm::Instruction& inst, std::uint32_t pc,
            std::uint32_t word, std::uint64_t before, bool interworks)
        {
            // PC is scratch until retirement; fixed-PC exits restore it.
            // The decoder excludes flag-writing exception-return forms.
            integer_.alu(inst, pc);
            code_.MOV(W17, W15);
            if (interworks)
                guard_exchange_target(pc, word, before);
        }
        void alu_branch(const arm::Instruction& inst, std::uint32_t pc,
            std::uint64_t ticks, bool interworks, bool backedge = true)
        {
            if (interworks)
                exchange(inst, pc, ticks, backedge);
            else {
                code_.AND(W15, W17, thumb_ ? ~1U : ~3U);
                code_.B(exit(StopReason::None, ticks, 0, { }, 0, true, { }, backedge));
            }
        }
        bool fast_indirect(const arm::Instruction& inst, std::uint32_t pc,
            std::uint32_t word, std::uint64_t before, std::uint64_t ticks,
            unsigned next_it, bool interworks)
        {
            if (!backedge_.accepts(StopReason::None, ticks, true, next_it, ticks))
                return false;
            if (inst.condition != 14)
                conditional_skip(inst.condition,
                    exit(StopReason::None, ticks, pc + inst.size,
                        { }, 0, false, next_it));
            exchanges_.emit(inst, pc, word, before, ticks, it_, next_it,
                backedge_.pc(), interworks);
            return true;
        }
        bool invariant_indirect() const { return exchanges_.invariant(); }
        void invalidate_written_registers(const arm::Instruction& inst)
        {
            if (inst.kind != arm::InstructionKind::ParallelSaturating)
                integer_.clear_parallel_values();
            // Static branches leave X17 intact; ALU emission revokes scratch
            // knowledge on its general paths. Indirect PC writes still clear.
            if (inst.kind != arm::InstructionKind::Multiply &&
                inst.kind != arm::InstructionKind::Branch &&
                (inst.kind != arm::InstructionKind::DataProcessing ||
                    arm::alu_writes_pc(inst)))
                integer_.clear_multiply_addend();
            const auto invalidate = [&](unsigned reg) {
                memory_.invalidate_register(reg);
                exchanges_.invalidate(reg);
                integer_.invalidate_multiply_addend(reg);
            };
            if (inst.kind == arm::InstructionKind::FloatingSystem) {
                const auto& v = std::get<arm::FloatingSystemOperands>(inst.vector);
                if (v.to_core && v.core != 15)
                    invalidate(v.core);
            }
            if (inst.kind == arm::InstructionKind::VectorCoreMove) {
                const auto& v = std::get<arm::VectorCoreMoveOperands>(inst.vector);
                if (v.to_core) {
                    invalidate(v.core);
                    if (v.pair)
                        invalidate(v.core2);
                }
            }
            if ((inst.kind == arm::InstructionKind::DataProcessing &&
                    (inst.opcode < 8 || inst.opcode > 11)) ||
                inst.kind == arm::InstructionKind::Multiply ||
                inst.kind == arm::InstructionKind::IntegerDivide ||
                inst.kind == arm::InstructionKind::PackHalfword ||
                inst.kind == arm::InstructionKind::IntegerTransform ||
                inst.kind == arm::InstructionKind::ParallelSaturating ||
                inst.kind == arm::InstructionKind::WideImmediate ||
                inst.kind == arm::InstructionKind::Swap ||
                ((inst.kind == arm::InstructionKind::Transfer ||
                     inst.kind == arm::InstructionKind::DualTransfer) && inst.load) ||
                (inst.kind == arm::InstructionKind::SystemControl && inst.load))
                invalidate(inst.rd);
            if (inst.kind == arm::InstructionKind::DualTransfer && inst.load)
                invalidate(inst.rs);
            if (inst.kind == arm::InstructionKind::Multiply &&
                arm::multiply_writes_pair(inst))
                invalidate(inst.rn);
            if ((inst.kind == arm::InstructionKind::Transfer ||
                    inst.kind == arm::InstructionKind::DualTransfer ||
                    inst.kind == arm::InstructionKind::VectorTransfer) &&
                inst.writeback)
                invalidate(inst.rn);
            if (inst.kind == arm::InstructionKind::MultipleTransfer) {
                memory_.invalidate_all();
                exchanges_.invalidate_all();
            }
            if ((inst.kind == arm::InstructionKind::Branch ||
                    inst.kind == arm::InstructionKind::BranchExchange) &&
                inst.link)
                invalidate(14);
        }
        VectorCodeGenerator& code() { return code_; }
        void vector_transfer(const arm::Instruction& inst, std::uint32_t pc,
            std::uint32_t word, std::uint64_t before, std::uint64_t cost,
            unsigned before_it, bool big_endian)
        {
            VectorMemoryEmitter { code_, big_endian, memory_.addresses(),
                vectors_ }
                .emit(inst, pc, cost,
                    exit(static_cast<StopReason>(vector_exit), before, pc,
                        word, 0, false, before_it));
        }
        VectorRegisters& vectors() { return vectors_; }
        bool needs_floating_guard(std::uint32_t mask) const
        {
            return (mask & ~floating_checked_) != 0;
        }
        void floating_guard(Label& failure, unsigned condition,
            std::uint32_t mask)
        {
            // An unconditional check covers later operations until a VMSR
            // invalidates it, including a conditionally executed VMSR.
            mask &= ~floating_checked_;
            if (condition == 14)
                floating_checked_ |= mask;
            code_.LDR(W16, X19, offsetof(CpuThreadState, fpscr));
            code_.MOV(W17, mask);
            code_.AND(W16, W16, W17);
            code_.CBNZ(W16, failure);
        }
        void parallel_saturating(const arm::Instruction& inst)
        {
            // Host SIMD saturation sets FPSR.QC. The existing environment
            // restores host state and merges only IEEE flags, not guest QC/Q/GE.
            floating_.use();
            integer_.parallel_saturating(inst);
        }
        void arithmetic(const arm::FloatingArithmeticOperands& v)
        {
            floating_.use();
            FloatingEmitter { code_, vectors_ }.arithmetic(v);
        }
        void comparison(const arm::FloatingComparisonOperands& v, bool preserve_apsr)
        {
            floating_.use();
            floating_.use_status();
            FloatingEmitter { code_, vectors_ }.comparison(v, preserve_apsr);
        }
        void conversion(const arm::FloatingConversionOperands& v)
        {
            floating_.use();
            FloatingEmitter { code_, vectors_ }.conversion(v);
        }
        void floating_system(const arm::FloatingSystemOperands& v,
            FloatingExceptionProfile profile, bool comparison_flags)
        {
            floating_.use_status();
            if (v.to_core && v.core == 15) {
                if (!comparison_flags)
                    code_.MSR(SystemReg::NZCV, X29);
                return;
            }
            floating_.use();
            if (v.to_core) {
                floating_.synchronize(code_);
                code_.LDR(WReg { static_cast<int>(v.core) }, X19,
                    offsetof(CpuThreadState, fpscr));
            } else {
                code_.MOV(W17, arm::fpscr_write_mask(profile));
                code_.AND(W16, WReg { static_cast<int>(v.core) }, W17);
                code_.STR(W16, X19, offsetof(CpuThreadState, fpscr));
                floating_.resume(code_);
                floating_checked_ = 0;
            }
        }
        IntegerEmitter& integer() { return integer_; }
        class InstructionMemoryExits final : public MemoryExits {
        public:
            InstructionMemoryExits(Emitter& owner, std::uint32_t pc,
                std::uint32_t word, std::uint64_t before,
                std::uint64_t cost, unsigned it, StopReason checked)
                : owner_(owner)
                , pc_(pc)
                , word_(word)
                , it_(it)
                , before_(before)
                , cost_(cost)
                , checked_(checked)
            {
            }
            Label& get(MemoryExits::Kind kind) override
            {
                auto& label = labels_[static_cast<unsigned>(kind)];
                if (!label) {
                    switch (kind) {
                    case MemoryExits::Kind::Checked:
                        label = &owner_.exit(
                            checked_, before_, pc_, word_, 0, false, it_);
                        break;
                    case MemoryExits::Kind::Unsupported:
                        label = &owner_.exit(
                            StopReason::UnsupportedInstruction, before_,
                            pc_, word_, 0, false, it_);
                        break;
                    case MemoryExits::Kind::Branch:
                        label = &owner_.exit(StopReason::None,
                            before_ + cost_, 0, { }, 0, true);
                        break;
                    }
                }
                return *label;
            }

        private:
            Emitter& owner_;
            std::uint32_t pc_, word_;
            unsigned it_;
            std::uint64_t before_, cost_;
            StopReason checked_;
            std::array<Label*, 3> labels_ { };
        };

        void transfer(const arm::Instruction& inst, std::uint32_t pc,
            std::uint32_t word, std::uint64_t before, std::uint64_t cost,
            unsigned before_it)
        {
            InstructionMemoryExits exits { *this, pc, word, before,
                cost, before_it, static_cast<StopReason>(memory_exit) };
            memory_.emit(inst, pc, cost, exits);
        }

        void multiple(const arm::Instruction& inst, std::uint32_t pc,
            std::uint32_t word, std::uint64_t before, std::uint64_t cost,
            unsigned before_it, bool big_endian)
        {
            memory_.invalidate_all();
            InstructionMemoryExits exits { *this, pc, word, before,
                cost, before_it, static_cast<StopReason>(multiple_exit) };
            MultipleEmitter { code_, big_endian }.emit(inst, pc, cost, exits);
        }

    private:
        WReg outcome_constant(std::uint32_t value, WReg temporary)
        {
            if (value == 0)
                return WZR;
            code_.MOV(temporary, value);
            return temporary;
        }
        void publish_exit(const Exit& out)
        {
            code_.ADD(X16, X24, X16);
            code_.STR(X16, X20, offsetof(NativeOutcome, ticks));
            if (!out.dynamic_pc)
                code_.MOV(W15, out.state_pc);
            code_.STR(W15, X19, 60);
            if (out.dynamic_pc || out.report_pc == out.state_pc)
                code_.STR(W15, X20, offsetof(NativeOutcome, pc));
            else
                code_.STR(outcome_constant(out.report_pc, W16), X20,
                    offsetof(NativeOutcome, pc));
            if (out.reason)
                code_.STR(outcome_constant(
                    static_cast<std::uint32_t>(*out.reason), W16), X20,
                    offsetof(NativeOutcome, reason));
            static_assert(offsetof(NativeOutcome, svc) ==
                offsetof(NativeOutcome, word) + sizeof(std::uint32_t));
            const auto word = outcome_constant(out.word, W16);
            const auto svc = outcome_constant(out.svc, W17);
            code_.STP(word, svc, X20, offsetof(NativeOutcome, word));
            code_.STR(outcome_constant(out.instruction ? 1U : 0U, W16),
                X20, offsetof(NativeOutcome, has_instruction));
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
        bool accepts_backedge(const Exit& out) const
        {
            return out.backedge &&
                backedge_.accepts(out.reason, out.ticks, out.dynamic_pc,
                    out.it, trace_.maximum_ticks);
        }
        CompiledTrace& trace_;
        VectorCodeGenerator code_;
        VectorRegisters vectors_;
        FloatingEnvironment floating_;
        std::uint32_t floating_checked_ = 0;
        IntegerEmitter integer_;
        ExchangePaths exchanges_;
        MemoryEmitter memory_;
        bool thumb_;
        bool continued_cycle_ = false;
        DynamicBackedge backedge_;
        std::size_t entry_end_ = 0, cycle_end_ = 0, first_cycle_words_ = 0;
        unsigned cycle_growth_ = 0, first_cycle_growth_ = 0;
        unsigned it_ = 0, size_ = 4;
        Label head_, loop_tail_, epilogue_;
        std::pmr::deque<Exit> exits_;
    };
}

CompiledTrace compile(InstructionSource& source, std::uint32_t pc,
    bool single_step, std::uint64_t maximum_ticks, bool big_endian, bool thumb,
    unsigned it, CycleExpansion expansion)
{
    const auto region = source.instruction_region(pc);
    if (region.begin > pc || region.end <= pc ||
        region.end > (std::uint64_t { 1 } << 32U))
        throw std::invalid_argument("invalid instruction region");
    // Scratch containers stay in this call. Returned machine code keeps its
    // ordinary allocator and cannot retain a pointer into this buffer.
    alignas(std::max_align_t) std::array<std::byte, 12U * 1024U> scratch;
    std::pmr::monotonic_buffer_resource temporary {
        scratch.data(), scratch.size(), std::pmr::new_delete_resource() };
    InstructionWindow window(source, region, thumb, &temporary,
        !single_step && expansion != CycleExpansion::Compact);
    const bool cache_reads = !single_step && expansion == CycleExpansion::Hot &&
        window.single_dependent_read_cycle(
            pc, it, maximum_trace_instructions, maximum_ticks);
    CompiledTrace out;
    auto first = window.take(pc, it, true);
    // Standalone unconditional stops share portable retirement and need no
    // executable allocation. Conditional or combined traces still use code.
    if (first && first->word && first->instruction.condition == 14) {
        if (const auto stop = arm::ArchitecturalStop::from(first->instruction)) {
            const auto cost = window.timing(*first);
            if (cost != 0 && cost <= maximum_ticks) {
                out.stop = stop;
                out.maximum_ticks = cost;
                out.instructions = 1;
                out.ends_with_stop = true;
                out.first_instruction = first->word;
                return out;
            }
        }
    }
    // Source capabilities and PC-write policy are fixed throughout a Run.
    const bool has_data_memory = source.data_memory() != nullptr;
    const bool alu_interworks =
        arm::alu_interworks(thumb, source.alu_pc_write_profile());
    // Larger read fallbacks are earned by the existing hot-cycle tier, so
    // short cold traces retain their compact code and translation cost.
    Emitter emit(out, big_endian, thumb, pc, it, single_step, &temporary,
        expansion == CycleExpansion::Hot, cache_reads);
    emit.prologue();
    const auto start = pc;
    const auto start_it = it;
    unsigned cycles = 0, cycle_length = 0, natural_copies = 0;
    unsigned original_natural_copies = 0;
    bool comparison_flags_live = false;
    const auto limit = single_step ? 1U : maximum_trace_instructions;
    for (unsigned n = 0; n < limit; ++n) {
        const bool comparison_flags = std::exchange(comparison_flags_live, false);
        if (pc < region.begin || pc >= region.end)
            break;
        emit.location(it, thumb ? 2U : 4U);
        auto decoded = n == 0 ? std::move(first) : window.take(pc, it, false);
        if (!decoded)
            break;
        const auto word = decoded->word;
        auto inst = decoded->instruction;
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
        if (straddles)
            emit.disable_backedges();
        emit.location(it, inst.size);
        const auto before = out.maximum_ticks;
        const bool unsupported =
            inst.kind == arm::InstructionKind::Unsupported ||
            ((inst.kind == arm::InstructionKind::TableBranch ||
                 inst.kind == arm::InstructionKind::Transfer ||
                 inst.kind == arm::InstructionKind::DualTransfer ||
                 inst.kind == arm::InstructionKind::ExclusiveTransfer ||
                 inst.kind == arm::InstructionKind::Swap ||
                 inst.kind == arm::InstructionKind::MultipleTransfer ||
                 inst.kind == arm::InstructionKind::VectorTransfer) &&
                !has_data_memory);
        if (inst.condition == 15 || (unsupported && inst.condition == 14)) {
            emit.code().B(emit.exit(
                StopReason::UnsupportedInstruction, before, pc, word));
            break;
        }
        if (unsupported)
            emit.code().B(static_cast<Cond>(inst.condition),
                emit.exit(
                    StopReason::UnsupportedInstruction, before, pc, word));
        const auto next_it = thumb ? (inst.kind == arm::InstructionKind::IfThen
                                             ? inst.immediate
                                             : arm::advance_it(it))
                                   : 0U;
        const auto floating_mask = inst.kind == arm::InstructionKind::FloatingImmediate
            ? arm::floating_vector_controls
            : inst.kind == arm::InstructionKind::FloatingComparison
            ? arm::floating_comparison_controls
            : inst.kind == arm::InstructionKind::FloatingConversion
            ? arm::floating_exception_enables
            : inst.kind == arm::InstructionKind::FloatingArithmetic
                ? arm::floating_arithmetic_controls(
                      std::get<arm::FloatingArithmeticOperands>(inst.vector).operation)
                : 0U;
        if (floating_mask != 0 && emit.needs_floating_guard(floating_mask)) {
            Label skipped;
            emit.conditional_skip(inst.condition, skipped);
            emit.floating_guard(emit.exit(StopReason::UnsupportedInstruction,
                before, pc, word), inst.condition, floating_mask);
            if (inst.condition != 14)
                emit.code().l(skipped);
        }
        const bool writes_pc = arm::alu_writes_pc(inst);
        const auto cost = window.timing(*decoded);
        const bool valid_timing = cost != 0 && cost <= maximum_ticks - before;
        Label exchange_failed;
        const bool fast_indirect =
            (inst.kind == arm::InstructionKind::BranchExchange || writes_pc) &&
            expansion == CycleExpansion::Hot &&
            valid_timing && emit.fast_indirect(inst, pc, *word, before,
                before + cost, next_it,
                inst.kind == arm::InstructionKind::BranchExchange || alu_interworks);
        if (inst.kind == arm::InstructionKind::BranchExchange &&
            !fast_indirect) {
            emit.conditional_skip(inst.condition, exchange_failed);
            emit.exchange_target(inst, pc, *word, before);
        }
        if (writes_pc && !fast_indirect) {
            Label skipped;
            emit.conditional_skip(inst.condition, skipped);
            emit.alu_target(inst, pc, *word, before, alu_interworks);
            if (inst.condition != 14)
                emit.code().l(skipped);
        }
        if (!valid_timing) {
            if (inst.kind == arm::InstructionKind::BranchExchange &&
                inst.condition != 14)
                emit.code().l(exchange_failed);
            emit.code().B(
                emit.exit(StopReason::InvalidTiming, before, pc, word));
            break;
        }
        if (!unsupported &&
            (inst.kind == arm::InstructionKind::TableBranch ||
                inst.kind == arm::InstructionKind::Transfer ||
                inst.kind == arm::InstructionKind::DualTransfer ||
                inst.kind == arm::InstructionKind::MultipleTransfer ||
                inst.kind == arm::InstructionKind::VectorTransfer))
            out.accesses_memory = true;
        out.maximum_ticks += cost;
        ++out.instructions;
        if (inst.kind == arm::InstructionKind::Breakpoint) {
            out.ends_with_stop = inst.condition == 14;
            emit.code().B(emit.exit(StopReason::Breakpoint, out.maximum_ticks,
                pc, word, 0, false, it));
            break;
        }
        emit.retired(next_it);
        const auto next_pc = pc + inst.size;
        const bool closes_natural_cycle = next_pc == start &&
            next_it == start_it && cycles + 1U >=
                (natural_copies != 0 ? natural_copies :
                    std::min(CycleTier::natural_copies(expansion),
                        limit / (cycle_length == 0 ? n + 1U : cycle_length)));
        // Short budgets and SingleStep use precise one-instruction entries.
        // A normal entry has no poll inside a proven register-only segment.
        // A cycle's final linear instruction also publishes its flags. Its
        // next producer belongs to a later group and may never execute.
        if (!single_step && !straddles && !closes_natural_cycle &&
            inst.set_flags &&
            window.flags_dead(inst, next_pc, next_it, limit - n - 1U,
                maximum_ticks - out.maximum_ticks))
            inst.set_flags = false;
        bool dynamic_end = false;
        if (inst.kind == arm::InstructionKind::BranchExchange) {
            if (!fast_indirect) {
                emit.exchange(inst, pc, out.maximum_ticks);
                if (inst.condition != 14)
                    emit.code().l(exchange_failed);
            }
            pc = next_pc;
            dynamic_end = true;
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
                out.ends_with_stop = true;
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
            if (!unsupported) {
                if (!fast_indirect)
                    emit.conditional_skip(inst.condition, skip);
                switch (inst.kind) {
                case arm::InstructionKind::FloatingSystem:
                    emit.floating_system(
                        std::get<arm::FloatingSystemOperands>(inst.vector),
                        source.floating_exception_profile(),
                        comparison_flags && inst.condition == 14);
                    break;
                case arm::InstructionKind::ClearExclusive:
                case arm::InstructionKind::Swap:
                case arm::InstructionKind::ExclusiveTransfer:
                    ExclusiveEmitter { emit.code() }.emit(inst, cost,
                        emit.exit(static_cast<StopReason>(exclusive_exit),
                            before, pc, word, 0, false, it));
                    break;
                case arm::InstructionKind::SystemControl:
                    SystemEmitter { emit.code() }.emit(inst);
                    if (arm::program_status_writes_endianness(inst))
                        emit.code().B(emit.exit(StopReason::None,
                            out.maximum_ticks, next_pc, { }, 0, false,
                            next_it, false));
                    if (inst.system ==
                        arm::SystemOperation::InstructionSynchronizationBarrier)
                        emit.code().B(emit.exit(StopReason::None,
                            out.maximum_ticks, next_pc));
                    break;
                case arm::InstructionKind::VectorTransfer:
                    emit.vector_transfer(
                        inst, pc, *word, before, cost, it, big_endian);
                    break;
                case arm::InstructionKind::Multiply:
                    emit.integer().multiply(inst);
                    break;
                case arm::InstructionKind::MultipleTransfer:
                    emit.multiple(
                        inst, pc, *word, before, cost, it, big_endian);
                    break;
                case arm::InstructionKind::TableBranch:
                case arm::InstructionKind::Transfer:
                case arm::InstructionKind::DualTransfer:
                    emit.transfer(inst, pc, *word, before, cost, it);
                    break;
                case arm::InstructionKind::WideImmediate:
                    emit.integer().wide_immediate(inst);
                    break;
                case arm::InstructionKind::PackHalfword:
                    emit.integer().pack_halfword(inst);
                    break;
                case arm::InstructionKind::ParallelSaturating:
                    emit.parallel_saturating(inst);
                    break;
                case arm::InstructionKind::IntegerTransform:
                    emit.integer().transform(inst);
                    break;
                case arm::InstructionKind::IntegerDivide:
                    emit.integer().divide(inst);
                    break;
                case arm::InstructionKind::FloatingArithmetic:
                    emit.arithmetic(
                        std::get<arm::FloatingArithmeticOperands>(inst.vector));
                    break;
                case arm::InstructionKind::FloatingImmediate:
                    FloatingEmitter { emit.code(), emit.vectors() }.immediate(
                        std::get<arm::FloatingImmediateOperands>(inst.vector), inst.condition == 14);
                    break;
                case arm::InstructionKind::FloatingComparison: {
                    const bool preserve_apsr = single_step || straddles ||
                        closes_natural_cycle ||
                        !window.flags_dead(0xfU, next_pc, next_it,
                            limit - n - 1U, maximum_ticks - out.maximum_ticks,
                            arm::floating_comparison_controls);
                    emit.comparison(
                        std::get<arm::FloatingComparisonOperands>(inst.vector),
                        preserve_apsr);
                    // Only an adjacent unconditional VMRS can inherit these
                    // hardware flags. Other instructions revoke the shortcut.
                    comparison_flags_live = !preserve_apsr && inst.condition == 14;
                    break;
                }
                case arm::InstructionKind::FloatingConversion:
                    emit.conversion(
                        std::get<arm::FloatingConversionOperands>(inst.vector));
                    break;
                case arm::InstructionKind::VectorCoreMove:
                    SimdEmitter { emit.code(), emit.vectors() }.core_move(
                        std::get<arm::VectorCoreMoveOperands>(inst.vector));
                    break;
                case arm::InstructionKind::VectorDuplicate:
                    SimdEmitter { emit.code(), emit.vectors() }.duplicate(
                        std::get<arm::VectorDuplicateOperands>(inst.vector));
                    break;
                case arm::InstructionKind::VectorImmediate:
                    SimdEmitter { emit.code(), emit.vectors() }.immediate(
                        std::get<arm::VectorImmediateOperands>(inst.vector));
                    break;
                case arm::InstructionKind::VectorBitwise:
                    SimdEmitter { emit.code(), emit.vectors() }.bitwise(
                        std::get<arm::VectorBitwiseOperands>(inst.vector));
                    break;
                case arm::InstructionKind::VectorExtract:
                    SimdEmitter { emit.code(), emit.vectors() }.extract(
                        std::get<arm::VectorExtractOperands>(inst.vector));
                    break;
                case arm::InstructionKind::DataProcessing:
                    if (writes_pc) {
                        if (!fast_indirect)
                            emit.alu_branch(
                                inst, pc, out.maximum_ticks, alu_interworks);
                    } else
                        emit.integer().alu(inst, pc);
                    break;
                default:
                    break;
                }
                if (inst.condition != 14 && !fast_indirect)
                    emit.code().l(skip);
            }
            pc = next_pc;
            if (DynamicBackedge::ends_trace(inst)) {
                dynamic_end = true;
            }
            if ((inst.kind == arm::InstructionKind::ExclusiveTransfer ||
                 inst.kind == arm::InstructionKind::Swap ||
                 inst.kind == arm::InstructionKind::ClearExclusive) && inst.condition == 14)
                break;
            // ISB ends the instruction view even when conditional. A false
            // condition follows the normal retirement path without a barrier.
            if (inst.kind == arm::InstructionKind::SystemControl &&
                inst.system ==
                    arm::SystemOperation::InstructionSynchronizationBarrier)
                break;
        }
        emit.invalidate_written_registers(inst);
        it = next_it;
        if (straddles)
            break;
        if (dynamic_end) {
            if (!single_step && expansion == CycleExpansion::Hot) {
                if (cycle_length == 0)
                    cycle_length = n + 1;
                // An invariant target is checked once per group, so copies
                // have the same code cost as a natural branch cycle.
                const auto maximum_copies = fast_indirect && emit.invariant_indirect()
                    ? CycleTier::maximum_copies : CycleTier::guarded_copies;
                const auto copies =
                    std::min(maximum_copies, limit / cycle_length);
                if (++cycles < copies &&
                    (fast_indirect || emit.continue_cycle(pc))) {
                    window.repeat_cycle();
                    pc = start;
                    it = start_it;
                    continue;
                }
            }
            if (fast_indirect)
                out.closed = true;
            break;
        }
        if (!single_step && pc == start && it == start_it) {
            ++cycles;
            if (cycle_length == 0) {
                cycle_length = n + 1;
                original_natural_copies = std::min(
                    CycleTier::natural_copies(expansion), limit / cycle_length);
                natural_copies = emit.expansion_copies(original_natural_copies);
            } else if (cycles == 2)
                natural_copies = emit.expansion_copies(original_natural_copies);
            const auto copies =
                std::min(natural_copies, limit / cycle_length);
            if (cycles >= copies) {
                out.closed = true;
                break;
            }
            window.repeat_cycle();
        }
    }
    emit.finish(pc);
    return out;
}
}
