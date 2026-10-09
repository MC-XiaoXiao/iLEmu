/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/factory.hpp"
#include "foundation/cpu.hpp"
#include "foundation/execution_memory.hpp"
#include <mutex>

namespace ilemu {
// Adapts the existing CPU scheduler contract, without exposing it to a backend.
// Slots own backends and memory views; Cpu owns every architectural register.
class IndependentCpuExecutor {
public:
    IndependentCpuExecutor(AddressSpace&, const ArmCpuModel&,
        execution::ExecutorFactory, std::size_t cache_bytes);
    CpuRunResult run(Cpu&, std::uint64_t ticks, bool single_step,
        bool cooperative, std::chrono::nanoseconds host_budget);
    void prepare();
    void clear_cache();
    void set_cache_size(std::size_t bytes);
    std::uint64_t code_cache_used();
    void halt(Dynarmic::HaltReason) noexcept;
    void clear_halt() noexcept;
    void request_guest_preemption() noexcept;
    void raise_memory_fault(std::uint32_t, std::size_t, MemoryPermission);

private:
    void ensure_executor();
    ExecutionMemory source_;
    execution::ExecutorFactory factory_;
    std::unique_ptr<execution::Executor> executor_;
    std::size_t cache_bytes_;
    // SVC/debug callbacks can clear caches at an already returned native
    // boundary.
    std::recursive_mutex mutex_;
    Cpu* owner_ = nullptr;
    std::optional<MemoryFault> callback_fault_;
    std::optional<std::chrono::steady_clock::time_point> preemption_started_;
};
}
