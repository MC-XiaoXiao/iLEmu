/* SPDX-License-Identifier: MPL-2.0 */
#include "arm64/executor.hpp"
#include "arm/t32_decode.hpp"
#include "code_cache.hpp"
#include "compiler.hpp"
#include "run_control.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ilemu::execution {
class Arm64Executor::Impl {
public:
    Impl(CodeAllocator& allocator, std::size_t budget)
        : cache_(allocator, budget, stats_)
    {
    }
    void clear() { cache_.clear(); }
    RunResult run(CpuThreadState& state, InstructionSource& source,
        const RunRequest& request)
    {
        if (!Arm64Executor::available())
            throw std::runtime_error("ARM64 executor requires an AArch64 host");
        cache_.begin_run();
        arm64::RunControl control { stops_, request };
        RunResult result;
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
            auto* selected = &cache_.entry(
                source, binding, result.pc, step, big_endian, thumb, it_state);
            const auto remaining =
                step ? UINT64_MAX : request.tick_budget - result.ticks_consumed;
            // Short budget tails execute a native one-instruction entry. This
            // preserves the interpreter's budget boundary without per-op polls.
            // Equality also prevents observing a fault immediately after the
            // final paid instruction, before the next budget check.
            if (!step && selected->maximum_ticks >= remaining)
                selected = &cache_.entry(source, binding, result.pc, true,
                    big_endian, thumb, it_state);
            const auto allowance = UINT64_MAX - result.ticks_consumed;
            if (selected->maximum_ticks > allowance)
                selected = &cache_.entry(source, binding, result.pc, true,
                    big_endian, thumb, it_state, allowance);
            arm64::NativeOutcome native;
            native.poll = arm64::RunControl::poll;
            native.control = &control;
            auto* memory = source.data_memory();
            if (memory && selected->accesses_memory)
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
            if (native.reason == arm64::memory_exit ||
                native.reason == arm64::multiple_exit) {
                if (!memory)
                    throw std::logic_error("memory exit has no bound memory");
                // No generated code or direct pointer remains in use. Release
                // physical code locks before a checked access can modify code.
                lease.reset();
                const bool memory_thumb = (state.cpsr & 0x20U) != 0;
                const auto memory_it = arm::it_state(state.cpsr);
                const bool multiple = native.reason == arm64::multiple_exit;
                const auto completion =
                    multiple
                        ? arm_memory::complete_multiple(
                              state, *memory, native.multiple)
                        : arm_memory::complete(state, *memory, native.transfer);
                native.reason = static_cast<std::uint32_t>(completion.reason);
                result.memory_fault = completion.fault;
                if (completion.reason == StopReason::None) {
                    if (memory_thumb)
                        state.cpsr = arm::with_it_state(
                            state.cpsr, arm::advance_it(memory_it));
                    native.ticks += multiple ? native.multiple.ticks
                                             : native.transfer.ticks;
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
    StopRequests stops_;
    Arm64Statistics stats_;
    arm64::CodeCache cache_;
};
Arm64Executor::Arm64Executor(CodeAllocator& allocator, std::size_t budget)
    : impl_(std::make_unique<Impl>(allocator, budget))
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
    impl_->stops_.request(reason);
}
void Arm64Executor::clear_stop() noexcept
{
    static_cast<void>(impl_->stops_.consume());
}
void Arm64Executor::clear_cache() { impl_->clear(); }
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
