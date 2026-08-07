#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "ilemu/address_space.hpp"
#include "ilemu/arm_cpu_model.hpp"

namespace ilemu {

class JitTranslationProfile;

struct CpuRunResult {
    Dynarmic::HaltReason reason{};
    std::uint64_t ticks_consumed{};
    std::optional<std::uint32_t> svc;
    std::uint64_t svc_calls{};
    std::optional<MemoryFault> fault;
    std::optional<std::uint32_t> debug_breakpoint;
    std::string exception;
};

enum class SvcDispatchMode : std::uint8_t {
    Immediate,
    Deferred,
};

struct CpuThreadState {
    std::array<std::uint32_t, 16> registers{};
    std::array<std::uint32_t, 64> extension_registers{};
    std::uint32_t cpsr{};
    std::uint32_t fpscr{};
    std::optional<std::uint32_t> cthread_self;
};

class CpuExecutionPool;
class JitExecutor;
class JitCallbacks;

class Cpu {
public:
    using SvcHandler = std::function<void(Cpu&, std::uint32_t)>;
    using MemoryWriteHandler = std::function<void(
        Cpu&, std::uint32_t, std::size_t, std::uint64_t)>;

    Cpu(std::size_t processor_id, AddressSpace& memory, Dynarmic::ExclusiveMonitor& monitor);
    ~Cpu();
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;

    CpuRunResult run(std::uint64_t ticks, std::size_t execution_slot = 0);
    CpuRunResult step(std::size_t execution_slot = 0);
    void reset();
    void clear_cache();
    void invalidate_cache_range(std::uint32_t address, std::size_t length);
    void clear_halt();
    void halt(Dynarmic::HaltReason reason = Dynarmic::HaltReason::UserDefined1);
    [[nodiscard]] Dynarmic::HaltReason consume_requested_halt_reason();

    [[nodiscard]] std::size_t processor_id() const { return processor_id_; }
    [[nodiscard]] std::array<std::uint32_t, 16>& registers();
    [[nodiscard]] const std::array<std::uint32_t, 16>& registers() const;
    [[nodiscard]] std::uint32_t cpsr() const;
    void set_cpsr(std::uint32_t value);
    [[nodiscard]] std::array<std::uint32_t, 64>& extension_registers();
    [[nodiscard]] const std::array<std::uint32_t, 64>&
    extension_registers() const;
    [[nodiscard]] std::uint32_t fpscr() const;
    void set_fpscr(std::uint32_t value);
    [[nodiscard]] std::optional<std::uint32_t> cthread_self() const;
    void set_cthread_self(std::optional<std::uint32_t> value);
    void set_svc_handler(SvcHandler handler);
    void set_svc_dispatch_mode(SvcDispatchMode mode);
    void set_memory_write_watchpoint(
        std::uint32_t address, MemoryWriteHandler handler);
    void set_debug_breakpoints_enabled(bool enabled);
    void set_translation_profile(
        std::shared_ptr<JitTranslationProfile> profile);
    // The scheduler calls this when a different guest thread is dispatched on
    // the same serialized virtual processor.
    void clear_exclusive_state(std::size_t execution_slot = 0);

private:
    friend class CpuCluster;
    friend class JitExecutor;
    friend class JitCallbacks;
    Cpu(
        std::size_t processor_id,
        std::shared_ptr<CpuExecutionPool> execution_pool);

    std::size_t processor_id_{};
    std::shared_ptr<CpuExecutionPool> execution_pool_;
    CpuThreadState state_;
    JitExecutor* active_executor_{};
    SvcHandler svc_handler_;
    MemoryWriteHandler memory_write_handler_;
    SvcDispatchMode svc_dispatch_mode_{SvcDispatchMode::Immediate};
    std::optional<std::uint32_t> memory_write_watch_address_;
    bool debug_breakpoints_enabled_{};
    Dynarmic::HaltReason requested_halt_reason_{};
};

class CpuCluster {
public:
    CpuCluster(std::size_t processor_count, AddressSpace& memory);
    CpuCluster(
        std::size_t initial_processor_count,
        std::size_t maximum_processor_count,
        AddressSpace& memory);
    // A serial guest scheduler can host many thread register contexts on one
    // emulated processor. Keeping those counts separate avoids making every
    // exclusive store scan all possible thread slots.
    CpuCluster(
        std::size_t initial_processor_count,
        std::size_t maximum_processor_count,
        AddressSpace& memory,
        bool serialized_execution);
    CpuCluster(
        std::size_t initial_processor_count,
        std::size_t maximum_processor_count,
        AddressSpace& memory,
        bool serialized_execution,
        const ArmCpuModel& cpu_model);
    CpuCluster(
        std::size_t initial_processor_count,
        std::size_t maximum_processor_count,
        AddressSpace& memory,
        std::size_t execution_slot_count,
        const ArmCpuModel& cpu_model);
    // Boot-created processes can share a Dynarmic monitor so LDREX/STREX
    // reservations at the same Guest address observe cross-process writes.
    // Each cluster receives a disjoint processor-id range in that monitor.
    CpuCluster(
        std::size_t initial_processor_count,
        std::size_t maximum_processor_count,
        AddressSpace& memory,
        std::size_t execution_slot_count,
        const ArmCpuModel& cpu_model,
        Dynarmic::ExclusiveMonitor& monitor,
        std::size_t monitor_processor_base);

    [[nodiscard]] std::size_t size() const { return cpus_.size(); }
    [[nodiscard]] std::size_t capacity() const {
        return maximum_processor_count_;
    }
    [[nodiscard]] Cpu& cpu(std::size_t index) { return *cpus_.at(index); }
    [[nodiscard]] const Cpu& cpu(std::size_t index) const { return *cpus_.at(index); }
    [[nodiscard]] std::optional<std::size_t> add_cpu();
    void set_process_id(std::uint32_t process_id);
    void set_jit_code_cache_size(std::size_t bytes);
    void clear_cache();
    void invalidate_cache_range(std::uint32_t address, std::size_t length);
    void set_translation_profile(
        std::shared_ptr<JitTranslationProfile> profile);
    std::size_t precompile_pending(
        std::size_t maximum_blocks, std::uint64_t budget_nanoseconds);
    // A dead guest task keeps its small register context until the parent
    // reaps the process, but no longer needs executable host code. Detach the
    // shared execution pool so its JIT caches can be destroyed off the
    // scheduler thread while the Cpu objects remain available as a zombie
    // task record.
    [[nodiscard]] std::shared_ptr<CpuExecutionPool>
    release_execution_resources();
    [[nodiscard]] bool has_execution_resources() const {
        return execution_pool_ != nullptr;
    }

    std::vector<CpuRunResult> run_parallel(std::uint64_t ticks_per_cpu);

private:
    AddressSpace* memory_{};
    std::size_t maximum_processor_count_{};
    bool serialized_execution_{};
    const ArmCpuModel* cpu_model_{};
    Dynarmic::ExclusiveMonitor monitor_;
    Dynarmic::ExclusiveMonitor* execution_monitor_{};
    std::size_t monitor_processor_base_{};
    std::shared_ptr<CpuExecutionPool> execution_pool_;
    std::vector<std::unique_ptr<Cpu>> cpus_;
};

}  // namespace ilemu
