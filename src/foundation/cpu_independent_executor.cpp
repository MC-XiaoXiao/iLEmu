/* SPDX-License-Identifier: MPL-2.0 */
#include "cpu_independent_executor.hpp"
#include "foundation/performance.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ilemu {
using execution::StopReason;
IndependentCpuExecutor::IndependentCpuExecutor(AddressSpace& memory,
    const ArmCpuModel& model, execution::ExecutorFactory factory,
    std::size_t cache_bytes)
    : source_(memory, model)
    , factory_(std::move(factory))
    , cache_bytes_(cache_bytes)
{
    if (!factory_)
        throw std::invalid_argument("independent executor factory is empty");
}
void IndependentCpuExecutor::ensure_executor()
{
    if (!executor_) {
        auto executor = factory_(cache_bytes_);
        if (!executor)
            throw std::runtime_error("executor factory returned no backend");
        executor_ = std::move(executor);
    }
}
void IndependentCpuExecutor::prepare()
{
    const std::lock_guard lock { mutex_ };
    ensure_executor();
}
void IndependentCpuExecutor::clear_cache()
{
    const std::lock_guard lock { mutex_ };
    if (executor_)
        executor_->clear_cache();
}
void IndependentCpuExecutor::set_cache_size(std::size_t bytes)
{
    const std::lock_guard lock { mutex_ };
    if (owner_)
        throw std::logic_error("cannot resize an active executor");
    if (cache_bytes_ != bytes) {
        executor_.reset();
        cache_bytes_ = bytes;
    }
}
std::uint64_t IndependentCpuExecutor::code_cache_used()
{
    const std::lock_guard lock { mutex_ };
    return executor_ ? executor_->retained_code_bytes() : 0;
}
void IndependentCpuExecutor::halt(Dynarmic::HaltReason reason) noexcept
{
    if (executor_)
        executor_->request_stop(
            Dynarmic::Has(reason, Dynarmic::HaltReason::UserDefined2)
                ? StopReason::GuestPreemption
                : StopReason::Cancelled);
}
void IndependentCpuExecutor::request_guest_preemption() noexcept
{
    if (!preemption_started_ &&
        performance_counters().cpu_source_diagnostics_enabled())
        preemption_started_ = std::chrono::steady_clock::now();
    halt(Dynarmic::HaltReason::UserDefined2);
}
void IndependentCpuExecutor::clear_halt() noexcept
{
    preemption_started_.reset();
    if (executor_)
        executor_->clear_stop();
}
void IndependentCpuExecutor::raise_memory_fault(
    std::uint32_t address, std::size_t size, MemoryPermission access)
{
    callback_fault_ = MemoryFault { address, size, access,
        "unmapped address or protection failure" };
    halt(Dynarmic::HaltReason::MemoryAbort);
}
CpuRunResult IndependentCpuExecutor::run(Cpu& cpu, std::uint64_t ticks,
    bool single_step, bool cooperative, std::chrono::nanoseconds host_budget)
{
    const std::lock_guard lock { mutex_ };
    if (owner_)
        throw std::logic_error("recursive CPU execution is not supported");
    ensure_executor();
    owner_ = &cpu;
    cpu.active_independent_executor_ = this;
    struct Detach {
        Cpu& cpu;
        Cpu*& owner;
        ~Detach()
        {
            cpu.active_independent_executor_ = nullptr;
            owner = nullptr;
        }
    } detach { cpu, owner_ };
    executor_->clear_stop();
    callback_fault_.reset();
    static_cast<void>(source_.take_fault());
    source_.set_write_observer(
        cpu.memory_write_watch_address_ && cpu.memory_write_handler_
            ? ExecutionMemory::WriteObserver { [&cpu](std::uint32_t address,
                                                   unsigned size,
                                                   std::uint32_t value) {
                  if (!cpu.memory_write_watch_address_ ||
                      !cpu.memory_write_handler_)
                      return;
                  const auto watched = *cpu.memory_write_watch_address_;
                  if (watched >= address &&
                      std::uint64_t { watched } <
                          std::uint64_t { address } + size)
                      cpu.memory_write_handler_(cpu, address, size, value);
              } }
            : ExecutionMemory::WriteObserver { });
    CpuRunResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto max_deadline = std::chrono::steady_clock::time_point::max();
    const auto budget = std::max(host_budget, std::chrono::nanoseconds::zero());
    const auto deadline =
        cooperative && !single_step
            ? (budget >= max_deadline - started ? max_deadline
                                                : started + budget)
            : max_deadline;
    const auto translations = executor_->translation_count();
    std::uint64_t execution_ns = 0;
    for (;;) {
        if (callback_fault_) {
            result.fault = callback_fault_;
            result.reason = result.reason | Dynarmic::HaltReason::MemoryAbort;
            break;
        }
        if (cpu.requested_halt_reason_ != Dynarmic::HaltReason { }) {
            result.reason = result.reason | cpu.requested_halt_reason_;
            break;
        }
        const auto execution_started = std::chrono::steady_clock::now();
        const auto run = executor_->run(cpu.state_, source_,
            { ticks - std::min(ticks, result.ticks_consumed),
                single_step ? execution::ExecutionMode::SingleStep
                            : execution::ExecutionMode::Run,
                deadline });
        execution_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - execution_started)
                .count());
        if (run.ticks_consumed > UINT64_MAX - result.ticks_consumed)
            throw std::overflow_error("CPU execution tick count overflow");
        result.ticks_consumed += run.ticks_consumed;
        const auto has = [&](StopReason reason) {
            return execution::has_reason(run.reason, reason);
        };
        // Cpu::step reports the attempted step even when its fetch/access
        // faults. Backends separately report SingleStep only on retirement.
        if (single_step || has(StopReason::SingleStep))
            result.reason = result.reason | Dynarmic::HaltReason::Step;
        if (has(StopReason::FetchFault) || has(StopReason::DataFault)) {
            result.fault = source_.take_fault();
            if (!result.fault)
                result.fault =
                    MemoryFault { run.memory_fault ? run.memory_fault->address
                                                   : run.pc,
                        (cpu.state_.cpsr & 0x20U) != 0 ? 2U : 4U,
                        has(StopReason::FetchFault) ? MemoryPermission::Execute
                                                    : MemoryPermission::Read,
                        "instruction or memory access fault" };
            result.reason = result.reason | Dynarmic::HaltReason::MemoryAbort;
            performance_counters().record_page_fault();
        }
        if (has(StopReason::Breakpoint)) {
            if (cpu.debug_breakpoints_enabled_) {
                result.debug_breakpoint = run.pc;
                result.reason =
                    result.reason | Dynarmic::HaltReason::UserDefined7;
            } else {
                result.architectural_exception =
                    CpuException { CpuException::Kind::Breakpoint, run.pc };
                result.reason =
                    result.reason | Dynarmic::HaltReason::UserDefined3;
            }
        }
        if (has(StopReason::UnsupportedInstruction) ||
            has(StopReason::InvalidTiming)) {
            std::ostringstream message;
            message << "independent executor stopped at 0x" << std::hex
                    << run.pc;
            if (run.instruction)
                message << " instruction=0x" << *run.instruction;
            message << (has(StopReason::InvalidTiming)
                            ? " invalid timing"
                            : " unsupported instruction or CPU state");
            result.exception = message.str();
            result.reason = result.reason | Dynarmic::HaltReason::UserDefined3;
        }
        if (has(StopReason::GuestPreemption))
            result.reason = result.reason | Dynarmic::HaltReason::UserDefined2;
        if (has(StopReason::Cancelled))
            result.reason =
                result.reason |
                (cpu.requested_halt_reason_ != Dynarmic::HaltReason { }
                        ? cpu.requested_halt_reason_
                        : Dynarmic::HaltReason::UserDefined1);
        if (has(StopReason::HostDeadline)) {
            result.host_yielded = true;
            ++result.host_yield_checks;
        }
        if (run.svc) {
            result.svc = run.svc;
            ++result.svc_calls;
            performance_counters().record_svc();
            if (cpu.svc_dispatch_mode_ == SvcDispatchMode::Deferred ||
                !cpu.svc_handler_) {
                result.reason =
                    result.reason | Dynarmic::HaltReason::UserDefined2;
                break;
            }
            cpu.svc_handler_(cpu, *run.svc);
            if (cooperative && !single_step &&
                std::chrono::steady_clock::now() >= deadline) {
                result.host_yielded = true;
                ++result.host_yield_checks;
            }
        }
        if (callback_fault_ ||
            cpu.requested_halt_reason_ != Dynarmic::HaltReason { })
            continue;
        if (single_step || result.reason != Dynarmic::HaltReason { } ||
            result.host_yielded || has(StopReason::TickBudget) ||
            result.ticks_consumed >= ticks)
            break;
        if (!run.svc)
            throw std::logic_error(
                "independent executor returned without a boundary");
    }
    result.translated_code = executor_->translation_count() != translations;
    result.host_execution_ns = execution_ns;
    if (Dynarmic::Has(result.reason, Dynarmic::HaltReason::UserDefined2) &&
        Dynarmic::Has(
            cpu.requested_halt_reason_, Dynarmic::HaltReason::UserDefined2)) {
        performance_counters().record_scheduler_preemption_return();
        if (preemption_started_) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - *preemption_started_)
                    .count();
            performance_counters().record_latency(
                PerfLatencyKind::SchedulerPreemptionRequestToReturn,
                static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)));
        }
    }
    preemption_started_.reset();
    performance_counters().record_cpu_execution(result.ticks_consumed);
    performance_counters().record_jit_host_yield(
        result.host_yield_checks, result.host_yielded);
    return result;
}
}
