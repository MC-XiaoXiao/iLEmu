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

#include "ilegacysim/address_space.hpp"
#include "ilegacysim/arm_cpu_model.hpp"

namespace ilegacysim {

struct CpuRunResult {
    Dynarmic::HaltReason reason{};
    std::uint64_t ticks_consumed{};
    std::optional<std::uint32_t> svc;
    std::optional<MemoryFault> fault;
    std::optional<std::uint32_t> debug_breakpoint;
    std::string exception;
};

enum class SvcDispatchMode : std::uint8_t {
    Immediate,
    Deferred,
};

class Cpu {
public:
    using SvcHandler = std::function<void(Cpu&, std::uint32_t)>;
    using MemoryWriteHandler = std::function<void(
        Cpu&, std::uint32_t, std::size_t, std::uint64_t)>;

    Cpu(std::size_t processor_id, AddressSpace& memory, Dynarmic::ExclusiveMonitor& monitor);
    ~Cpu();
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;

    CpuRunResult run(std::uint64_t ticks);
    CpuRunResult step();
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
    void set_svc_handler(SvcHandler handler);
    void set_svc_dispatch_mode(SvcDispatchMode mode);
    void set_memory_write_watchpoint(
        std::uint32_t address, MemoryWriteHandler handler);
    void set_debug_breakpoints_enabled(bool enabled);
    // The scheduler calls this when a different guest thread is dispatched on
    // the same serialized virtual processor.
    void clear_exclusive_state();

private:
    friend class CpuCluster;
    Cpu(
        std::size_t processor_id,
        std::size_t exclusive_processor_id,
        AddressSpace& memory,
        Dynarmic::ExclusiveMonitor& monitor,
        const ArmCpuModel& cpu_model);
    class Callbacks;
    void ensure_jit();

    std::size_t processor_id_{};
    std::size_t exclusive_processor_id_{};
    Dynarmic::ExclusiveMonitor* monitor_{};
    std::unique_ptr<Callbacks> callbacks_;
    std::unique_ptr<Dynarmic::A32::Jit> jit_;
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

    [[nodiscard]] std::size_t size() const { return cpus_.size(); }
    [[nodiscard]] std::size_t capacity() const {
        return maximum_processor_count_;
    }
    [[nodiscard]] Cpu& cpu(std::size_t index) { return *cpus_.at(index); }
    [[nodiscard]] const Cpu& cpu(std::size_t index) const { return *cpus_.at(index); }
    [[nodiscard]] std::optional<std::size_t> add_cpu();

    std::vector<CpuRunResult> run_parallel(std::uint64_t ticks_per_cpu);

private:
    AddressSpace* memory_{};
    std::size_t maximum_processor_count_{};
    bool serialized_execution_{};
    const ArmCpuModel* cpu_model_{};
    Dynarmic::ExclusiveMonitor monitor_;
    std::vector<std::unique_ptr<Cpu>> cpus_;
};

}  // namespace ilegacysim
