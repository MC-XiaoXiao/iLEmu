/* SPDX-License-Identifier: MPL-2.0 */
#include "arm64/executor.hpp"
#include "execution/context.hpp"
#include "arm/t32_decode.hpp"
#include "arm_interpreter/interpreter.hpp"
#include "code_cache.hpp"
#include "compiler.hpp"
#include "run_control.hpp"
#include "path_recorder.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ilemu::execution {
class Arm64Executor::Impl {
public:
    Impl(CodeAllocator& allocator, std::size_t budget,
        std::shared_ptr<SharedCompilationCache> shared)
        : cache_(allocator, budget, stats_, std::move(shared))
    {
    }
    void clear() { cache_.clear(); }
    void invalidate_range(std::uint32_t address, std::size_t size)
    {
        cache_.invalidate_range(address, size);
    }
    RunResult run(CpuThreadState& state, InstructionSource& source,
        const RunRequest& request)
    {
        if (!Arm64Executor::available())
            throw std::runtime_error("ARM64 executor requires an AArch64 host");
        context_.bind_memory(source.data_memory());
        cache_.begin_run();
        arm64::RunControl control { context_.stops, request };
        RunResult result;
        arm64::PathRecorder recorder;
        for (;;) {
            result.pc = state.registers[15];
            result.instruction.reset();
            result.memory_fault.reset();
            result.reason = control.poll();
            const bool step = request.mode == ExecutionMode::SingleStep;
            if (!step && result.ticks_consumed >= request.tick_budget)
                result.reason = result.reason | StopReason::TickBudget;
            if (result.reason != StopReason::None)
                return result;
            if (recorder.active()) {
                // Recording needs the retired successor, not executable code
                // for each observation. Share reservations and cumulative
                // ticks with the native backend across this bounded slice.
                const auto before = result.ticks_consumed;
                result = ArmInterpreter::run_slice(
                    state, source, request, context_, 1, before);
                if (result.reason != StopReason::None)
                    return result;
                if (result.ticks_consumed != before) {
                    ++stats_.path_recording_steps;
                    recorder.retired(state);
                } else
                    recorder.clear();
                continue;
            }
            const bool thumb = (state.cpsr & 0x20U) != 0;
            const auto it_state = arm::it_state(state.cpsr);
            if ((state.cpsr & (0x1fU | 0x01000000U)) != 0x10U ||
                (!thumb && it_state != 0)) {
                result.reason = StopReason::UnsupportedInstruction;
                return result;
            }
            if ((result.pc & (thumb ? 1U : 3U)) != 0) {
                result.reason = StopReason::FetchFault;
                return result;
            }
            auto lease = source.acquire_code_lease(result.pc, thumb);
            if (!lease)
                throw std::runtime_error(
                    "ARM64 executor requires a stable instruction lease");
            if (auto fault = source.prepare_instruction_fetch(
                    result.pc, thumb ? 2U : 4U)) {
                result.memory_fault = fault;
                result.reason = StopReason::FetchFault;
                return result;
            }
            if (thumb) {
                const auto first = source.fetch16(result.pc);
                if (!first) {
                    result.reason = StopReason::FetchFault;
                    return result;
                }
                if (arm::thumb_is_wide(*first)) {
                    if (auto fault = source.prepare_instruction_fetch(
                            result.pc + 2U, 2U)) {
                        result.memory_fault = fault;
                        result.reason = StopReason::FetchFault;
                        return result;
                    }
                }
            }
            const auto binding = cache_.bind(*lease);
            const bool big_endian = (state.cpsr & (1U << 9U)) != 0;
            const auto path = recorder.ready(binding.identity, binding.generation, state);
            auto* selected = &cache_.entry(
                source, binding, result.pc, step, big_endian, thumb, it_state,
                request.translation_observer, UINT64_MAX, path);
            if (recorder.complete())
                recorder.clear();
            if (!step && selected->wants_path()) {
                selected->defer_path();
                const auto view = lease->cached_continuation(result.pc);
                if (view && view->inspectable &&
                    recorder.begin(binding.identity, binding.generation, state)) {
                    ++stats_.path_recording_attempts;
                    // Leave this scope before checked execution can write to
                    // code: its native instruction lease must be released.
                    continue;
                }
            }
            const auto remaining =
                step ? UINT64_MAX : request.tick_budget - result.ticks_consumed;
            // Short budget tails execute a native one-instruction entry. This
            // preserves the interpreter's budget boundary without per-op polls.
            // At equality an unconditional paid stop returns before fetching
            // a successor. Other traces must not observe a successor fault
            // before the next budget check.
            if (!step && selected->maximum_ticks >= remaining &&
                (!selected->ends_with_stop || selected->maximum_ticks != remaining))
                selected = &cache_.entry(source, binding, result.pc, true,
                    big_endian, thumb, it_state, request.translation_observer);
            const auto allowance = UINT64_MAX - result.ticks_consumed;
            if (selected->maximum_ticks > allowance)
                selected = &cache_.entry(source, binding, result.pc, true,
                    big_endian, thumb, it_state, request.translation_observer,
                    allowance);
            arm64::NativeOutcome native;
            native.thread_pointer_read_only = state.cthread_self.value_or(0);
            native.poll = arm64::RunControl::poll;
            native.control = &control;
            auto* memory = source.data_memory();
            arm64::CodeCache::ContinuationScope scope { cache_, *lease, native, memory };
            const bool continuations = !step && cache_.prepare_continuations(
                *selected, scope, remaining);
            if (memory && (selected->accesses_memory || continuations))
                native.memory = memory->direct_memory();
            if (selected->closed && selected->maximum_ticks != 0) {
                native.groups = std::min<std::uint64_t>(
                    1024, remaining / selected->maximum_ticks);
                native.groups = std::max<std::uint64_t>(1, native.groups);
            }
            // Compilation and page binding can outlast a short deadline or
            // receive a stop request. Do not execute a batch before rechecking.
            result.reason = control.poll();
            if (result.reason != StopReason::None)
                return result;
            if (selected->stop) {
                result.instruction = selected->first_instruction;
                selected->stop->retire(state, result, thumb, it_state,
                    selected->maximum_ticks, request.mode);
                return result;
            }
#if defined(__aarch64__) || defined(_M_ARM64)
            using Function = void (*)(CpuThreadState*, arm64::NativeOutcome*);
            const auto function = reinterpret_cast<Function>(
                const_cast<void*>(selected->code->entry()));
            function(&state, &native);
#else
            throw std::runtime_error(
                "ARM64 executor is unavailable on this host");
#endif
            ++stats_.execution_calls;
            stats_.continuation_calls += native.continuation_calls;
            stats_.cross_region_calls += native.cross_region_calls;
            if (continuations)
                cache_.finish_continuations(native);
            else
                cache_.observe(*selected, native.ticks);
            if (native.continuation_stopped) {
                result.ticks_consumed += native.ticks;
                result.reason = static_cast<StopReason>(native.reason);
                result.pc = state.registers[15];
                return result;
            }
            if (native.reason == arm64::exclusive_exit ||
                native.reason == arm64::memory_exit ||
                native.reason == arm64::multiple_exit ||
                native.reason == arm64::vector_exit) {
                if (!memory && native.reason != arm64::exclusive_exit)
                    throw std::logic_error("memory exit has no bound memory");
                // No generated code or direct pointer remains in use. Release
                // physical code locks before a checked access can modify code.
                lease.reset();
                const bool memory_thumb = (state.cpsr & 0x20U) != 0;
                const auto memory_it = arm::it_state(state.cpsr);
                arm_memory::Completion completion;
                std::uint64_t transfer_ticks;
                if (native.reason == arm64::exclusive_exit) {
                    completion = arm_memory::complete_exclusive(
                        state, memory, context_.exclusive, native.exclusive);
                    transfer_ticks = native.exclusive.ticks;
                } else if (native.reason == arm64::vector_exit) {
                    completion = arm_memory::complete_vector(
                        state, *memory, native.vector);
                    transfer_ticks = native.vector.ticks;
                } else if (native.reason == arm64::multiple_exit) {
                    completion = arm_memory::complete_multiple(
                        state, *memory, native.multiple);
                    transfer_ticks = native.multiple.ticks;
                } else {
                    completion = arm_memory::complete(
                        state, *memory, native.transfer);
                    transfer_ticks = native.transfer.ticks;
                }
                native.reason = static_cast<std::uint32_t>(completion.reason);
                result.memory_fault = completion.fault;
                if (completion.reason == StopReason::None) {
                    if (memory_thumb)
                        state.cpsr = arm::with_it_state(
                            state.cpsr, arm::advance_it(memory_it));
                    native.ticks += transfer_ticks;
                    native.pc = state.registers[15];
                    native.has_instruction = step ? 1U : 0U;
                }
            }
            result.ticks_consumed += native.ticks;
            result.reason = static_cast<StopReason>(native.reason);
            result.pc = native.pc;
            if (native.has_instruction)
                result.instruction = native.word;
            if (has_reason(result.reason, StopReason::Svc))
                result.svc = native.svc;
            if (step && (result.reason == StopReason::None ||
                            has_reason(result.reason, StopReason::Svc) ||
                            has_reason(result.reason, StopReason::Breakpoint))) {
                result.reason = result.reason | StopReason::SingleStep;
                if (!result.instruction)
                    result.instruction = selected->first_instruction;
            }
            if (result.reason != StopReason::None)
                return result;
        }
    }
    ExecutorContext context_;
    Arm64Statistics stats_;
    arm64::CodeCache cache_;
};
Arm64Executor::Arm64Executor(CodeAllocator& allocator, std::size_t budget,
    std::shared_ptr<SharedCompilationCache> shared)
    : impl_(std::make_unique<Impl>(allocator, budget, std::move(shared)))
{
}
Arm64Executor::~Arm64Executor() = default;
bool Arm64Executor::available() noexcept
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return true;
#else
    return false;
#endif
}
RunResult Arm64Executor::run(
    CpuThreadState& state, InstructionSource& source, const RunRequest& request)
{
    return impl_->run(state, source, request);
}
void Arm64Executor::request_stop(StopReason reason) noexcept
{
    impl_->context_.stops.request(reason);
}
void Arm64Executor::clear_stop() noexcept
{
    static_cast<void>(impl_->context_.stops.consume());
}
void Arm64Executor::clear_cache() { impl_->clear(); }
void Arm64Executor::invalidate_cache_range(
    std::uint32_t address, std::size_t size)
{
    impl_->invalidate_range(address, size);
}
void Arm64Executor::clear_exclusive_state()
{ impl_->context_.clear_exclusive_state(); }
std::uint64_t Arm64Executor::retained_code_bytes() const noexcept
{
    return impl_->stats_.retained_bytes;
}
std::uint64_t Arm64Executor::translation_count() const noexcept
{
    return impl_->stats_.compiled_regions;
}
Arm64Statistics Arm64Executor::statistics() const noexcept
{
    return impl_->stats_;
}
}
