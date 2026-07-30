#include "ilemu/cpu.hpp"

#include <array>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/ir/basic_block.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "ilemu/jit_translation_profile.hpp"
#include "ilemu/performance.hpp"

namespace ilemu {
namespace {

[[nodiscard]] Dynarmic::A32::ArchVersion dynarmic_architecture_version(
    ArmArchitectureVersion version) {
    switch (version) {
    case ArmArchitectureVersion::Armv6K:
        return Dynarmic::A32::ArchVersion::v6K;
    case ArmArchitectureVersion::Armv7:
        return Dynarmic::A32::ArchVersion::v7;
    }
    throw std::invalid_argument{"unsupported ARM architecture version"};
}

template <typename Jit>
std::uint64_t jit_code_cache_used(const Jit& jit) {
    if constexpr (requires { jit.CodeCacheUsed(); }) {
        return static_cast<std::uint64_t>(jit.CodeCacheUsed());
    }
    return 0;
}

}  // namespace

class JitCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    JitCallbacks(
        AddressSpace& memory,
        const ArmCpuModel& cpu_model)
        : memory_{memory}, cpu_model_{cpu_model} {}

    void attach(Cpu* owner, Dynarmic::A32::Jit* jit) {
        owner_ = owner;
        jit_ = jit;
    }

    bool PreCodeReadHook(
        bool, Dynarmic::A32::VAddr, Dynarmic::A32::IREmitter& ir) override {
        if (ir.block.CycleCount() == 0) {
            performance_counters().record_translation_block();
        }
        // This fork's translator continues normal decoding when the hook returns
        // true. Returning false is reserved for a hook that already emitted an IR
        // terminal. (The comment in UserCallbacks currently says the opposite.)
        return true;
    }

    void CodeTranslationCompleted(
        std::uint64_t location_descriptor) noexcept override {
        if (translation_profile_) {
            translation_profile_->record(location_descriptor);
        }
    }

    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t address) override {
        const auto value = memory_.read32(address, MemoryPermission::Execute);
        if (!value) {
            memory_fault(address, 4, MemoryPermission::Execute);
        }
        return value;
    }

    std::uint8_t MemoryRead8(std::uint32_t address) override {
        return read<std::uint8_t>(address, &AddressSpace::read8);
    }
    std::uint16_t MemoryRead16(std::uint32_t address) override {
        return read<std::uint16_t>(address, &AddressSpace::read16);
    }
    std::uint32_t MemoryRead32(std::uint32_t address) override {
        return read<std::uint32_t>(address, &AddressSpace::read32);
    }
    std::uint64_t MemoryRead64(std::uint32_t address) override {
        return read<std::uint64_t>(address, &AddressSpace::read64);
    }

    void MemoryWrite8(std::uint32_t address, std::uint8_t value) override {
        write(address, value, &AddressSpace::write8);
    }
    void MemoryWrite16(std::uint32_t address, std::uint16_t value) override {
        write(address, value, &AddressSpace::write16);
    }
    void MemoryWrite32(std::uint32_t address, std::uint32_t value) override {
        write(address, value, &AddressSpace::write32);
    }
    void MemoryWrite64(std::uint32_t address, std::uint64_t value) override {
        write(address, value, &AddressSpace::write64);
    }

    std::uint8_t MemorySwap8(
        std::uint32_t address, std::uint8_t value) override {
        return swap(address, value, &AddressSpace::exchange8);
    }
    std::uint32_t MemorySwap32(
        std::uint32_t address, std::uint32_t value) override {
        return swap(address, value, &AddressSpace::exchange32);
    }

    bool MemoryWriteExclusive8(
        std::uint32_t address, std::uint8_t value, std::uint8_t expected) override {
        const auto written = memory_.compare_exchange8(address, expected, value);
        if (written) notify_memory_write(address, sizeof(value), value);
        return written;
    }
    bool MemoryWriteExclusive16(
        std::uint32_t address, std::uint16_t value, std::uint16_t expected) override {
        const auto written = memory_.compare_exchange16(address, expected, value);
        if (written) notify_memory_write(address, sizeof(value), value);
        return written;
    }
    bool MemoryWriteExclusive32(
        std::uint32_t address, std::uint32_t value, std::uint32_t expected) override {
        const auto written = memory_.compare_exchange32(address, expected, value);
        if (written) notify_memory_write(address, sizeof(value), value);
        return written;
    }
    bool MemoryWriteExclusive64(
        std::uint32_t address, std::uint64_t value, std::uint64_t expected) override {
        const auto written = memory_.compare_exchange64(address, expected, value);
        if (written) notify_memory_write(address, sizeof(value), value);
        return written;
    }

    bool IsReadOnlyMemory(std::uint32_t) override { return false; }

    void InterpreterFallback(std::uint32_t pc, std::size_t count) override {
        std::ostringstream message;
        message << "Dynarmic interpreter fallback at 0x" << std::hex << pc
                << " for " << std::dec << count << " instruction(s)";
        exception_ = message.str();
        jit_->HaltExecution(Dynarmic::HaltReason::UserDefined3);
    }

    void CallSVC(std::uint32_t immediate) override {
        performance_counters().record_svc();
        ++svc_calls_;
        svc_ = immediate;
        if (owner_->svc_dispatch_mode_ == SvcDispatchMode::Deferred) {
            jit_->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        if (owner_->svc_handler_) {
            owner_->svc_handler_(*owner_, immediate);
        } else {
            jit_->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        }
    }

    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override {
        if (exception == Dynarmic::A32::Exception::Breakpoint &&
            owner_->debug_breakpoints_enabled_) {
            breakpoint_ = pc;
            owner_->registers()[15] = pc;
            jit_->HaltExecution(Dynarmic::HaltReason::UserDefined7);
            return;
        }
        std::ostringstream message;
        message << "ARM exception " << static_cast<unsigned>(exception)
                << " at 0x" << std::hex << pc;
        exception_ = message.str();
        jit_->HaltExecution(Dynarmic::HaltReason::UserDefined3);
    }

    void AddTicks(std::uint64_t ticks) override {
        consumed_ += ticks;
        ticks_remaining_ = ticks >= ticks_remaining_ ? 0 : ticks_remaining_ - ticks;
    }
    std::uint64_t GetTicksRemaining() override { return ticks_remaining_; }
    std::uint64_t GetTicksForCode(
        bool is_thumb, Dynarmic::A32::VAddr address,
        std::uint32_t instruction) override {
        return cpu_model_.ticks_for_instruction(
            is_thumb, address, instruction);
    }

    void begin(std::uint64_t ticks) {
        ticks_remaining_ = ticks;
        consumed_ = 0;
        svc_.reset();
        svc_calls_ = 0;
        fault_.reset();
        breakpoint_.reset();
        exception_.clear();
    }

    CpuRunResult result(Dynarmic::HaltReason reason) const {
        return CpuRunResult{
            reason, consumed_, svc_, svc_calls_, fault_, breakpoint_,
            exception_};
    }

    [[nodiscard]] const ArmCpuModel& cpu_model() const {
        return cpu_model_;
    }
    [[nodiscard]] std::uint8_t** jit_read_page_table() {
        return memory_.jit_read_page_table();
    }
    [[nodiscard]] std::uint8_t** jit_write_page_table() {
        return memory_.jit_write_page_table();
    }
    void set_translation_profile(
        std::shared_ptr<JitTranslationProfile> profile) {
        translation_profile_ = std::move(profile);
    }
private:
    template<typename T, typename Member>
    T read(std::uint32_t address, Member member) {
        const auto value = (memory_.*member)(address, MemoryPermission::Read);
        if (!value) {
            memory_fault(address, sizeof(T), MemoryPermission::Read);
            return 0;
        }
        return *value;
    }

    template<typename T, typename Member>
    void write(std::uint32_t address, T value, Member member) {
        if (!(memory_.*member)(address, value)) {
            memory_fault(address, sizeof(T), MemoryPermission::Write);
        } else {
            notify_memory_write(address, sizeof(T), value);
        }
    }

    template<typename T, typename Member>
    T swap(std::uint32_t address, T value, Member member) {
        const auto previous = (memory_.*member)(address, value);
        if (!previous) {
            memory_fault(address, sizeof(T),
                         MemoryPermission::Read | MemoryPermission::Write);
            return 0;
        }
        notify_memory_write(address, sizeof(T), value);
        return *previous;
    }

    void notify_memory_write(
        std::uint32_t address, std::size_t size, std::uint64_t value) {
        if (!owner_->memory_write_watch_address_ ||
            !owner_->memory_write_handler_) {
            return;
        }
        const auto write_begin = static_cast<std::uint64_t>(address);
        const auto write_end = write_begin + size;
        const auto watched =
            static_cast<std::uint64_t>(*owner_->memory_write_watch_address_);
        if (watched >= write_begin && watched < write_end) {
            owner_->memory_write_handler_(*owner_, address, size, value);
        }
    }

    void memory_fault(std::uint32_t address, std::size_t size, MemoryPermission access) {
        performance_counters().record_page_fault();
        fault_ = MemoryFault{address, size, access, "unmapped address or protection failure"};
        if (jit_ != nullptr) {
            jit_->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
        }
    }

    AddressSpace& memory_;
    const ArmCpuModel& cpu_model_;
    Cpu* owner_{};
    Dynarmic::A32::Jit* jit_{};
    std::uint64_t ticks_remaining_{};
    std::uint64_t consumed_{};
    std::optional<std::uint32_t> svc_;
    std::uint64_t svc_calls_{};
    std::optional<MemoryFault> fault_;
    std::optional<std::uint32_t> breakpoint_;
    std::string exception_;
    std::shared_ptr<JitTranslationProfile> translation_profile_;
};

class JitExecutor {
public:
    JitExecutor(
        std::size_t processor_id,
        std::size_t execution_slot,
        AddressSpace& memory,
        Dynarmic::ExclusiveMonitor& monitor,
        const ArmCpuModel& cpu_model)
        : processor_id_{processor_id},
          execution_slot_{execution_slot},
          memory_{memory},
          monitor_{monitor},
          callbacks_{std::make_unique<JitCallbacks>(memory, cpu_model)} {}

    ~JitExecutor() {
        performance_counters().record_jit_code_cache_usage(
            process_id_, static_cast<std::uint32_t>(execution_slot_),
            recorded_jit_code_cache_bytes_, 0);
        if (jit_) {
            performance_counters().record_jit_destroyed();
        }
    }

    CpuRunResult run(Cpu& cpu, std::uint64_t ticks, bool single_step) {
        const std::scoped_lock lock{execution_mutex_};
        memory_.synchronize_shared_write_tracking();
        ensure_jit();
        load_state(cpu);
        callbacks_->begin(single_step ? 1 : ticks);
        try {
            const auto reason = single_step ? jit_->Step() : jit_->Run();
            auto result = callbacks_->result(reason);
            save_state(cpu);
            record_code_cache_usage();
            performance_counters().record_cpu_execution(result.ticks_consumed);
            return result;
        } catch (...) {
            save_state(cpu);
            record_code_cache_usage();
            throw;
        }
    }

    void clear_cache() {
        if (jit_) {
            jit_->ClearCache();
            record_code_cache_usage();
        }
    }

    void invalidate_cache_range(std::uint32_t address, std::size_t length) {
        if (jit_ && length != 0) {
            jit_->InvalidateCacheRange(address, length);
            record_code_cache_usage();
        }
    }

    void clear_halt() {
        if (jit_) {
            jit_->ClearHalt(all_halt_reasons());
        }
    }

    void halt(Dynarmic::HaltReason reason) {
        if (jit_) {
            jit_->HaltExecution(reason);
        }
    }

    void clear_exclusive_state() {
        ensure_jit();
        jit_->ClearExclusiveState();
    }

    void set_translation_profile(
        std::shared_ptr<JitTranslationProfile> profile) {
        const auto locations =
            profile ? profile->snapshot() : std::vector<std::uint64_t>{};
        {
            const std::lock_guard execution_lock{execution_mutex_};
            callbacks_->set_translation_profile(profile);
        }
        const std::lock_guard queue_lock{precompile_queue_mutex_};
        pending_precompile_entries_.clear();
        seen_precompile_entries_.clear();
        for (auto location = locations.rbegin();
             location != locations.rend() &&
             pending_precompile_entries_.size() <
                 jit_translation_profile_maximum_locations;
             ++location) {
            if (*location != 0 &&
                seen_precompile_entries_.insert(*location).second) {
                pending_precompile_entries_.push_back(*location);
            }
        }
    }

    std::size_t precompile_pending(
        std::size_t maximum_blocks, std::uint64_t budget_nanoseconds) {
        if (maximum_blocks == 0 || budget_nanoseconds == 0) {
            return 0;
        }
        std::size_t candidates_remaining = 0;
        {
            const std::lock_guard queue_lock{precompile_queue_mutex_};
            if (pending_precompile_entries_.empty()) {
                return 0;
            }
            candidates_remaining = pending_precompile_entries_.size();
        }
        const std::lock_guard execution_lock{execution_mutex_};
        memory_.synchronize_shared_write_tracking();
        ensure_jit();
        constexpr std::size_t cache_reserve = 8U * 1024U * 1024U;
        const auto started = std::chrono::steady_clock::now();
        std::size_t compiled = 0;
        while (compiled < maximum_blocks && candidates_remaining != 0) {
            if (jit_code_cache_used(*jit_) + cache_reserve >= code_cache_size_) {
                break;
            }
            std::optional<std::uint64_t> entry;
            {
                const std::lock_guard queue_lock{precompile_queue_mutex_};
                if (pending_precompile_entries_.empty()) {
                    break;
                }
                entry = pending_precompile_entries_.front();
                pending_precompile_entries_.pop_front();
            }
            --candidates_remaining;
            const auto pc = static_cast<std::uint32_t>(*entry);
            const auto code_address = pc & ~std::uint32_t{3};
            if (!memory_.accessible(
                    code_address, sizeof(std::uint32_t),
                    MemoryPermission::Execute)) {
                // A persisted profile can name a dylib before Guest dyld maps
                // it. Dynarmic would otherwise cache a NoExecute block which
                // remains stale after the mapping appears. Defer the hint
                // until its code page is executable instead.
                const std::lock_guard queue_lock{precompile_queue_mutex_};
                pending_precompile_entries_.push_back(*entry);
            } else {
                callbacks_->begin(0);
                jit_->Precompile(*entry);
                ++compiled;
            }
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started);
            if (static_cast<std::uint64_t>(elapsed.count()) >=
                budget_nanoseconds) {
                break;
            }
        }
        record_code_cache_usage();
        return compiled;
    }

    void reset_live_state() {
        ensure_jit();
        jit_->Reset();
    }

    [[nodiscard]] std::array<std::uint32_t, 16>& registers() {
        return jit_->Regs();
    }

    [[nodiscard]] const std::array<std::uint32_t, 16>& registers() const {
        return jit_->Regs();
    }

    [[nodiscard]] std::array<std::uint32_t, 64>& extension_registers() {
        return jit_->ExtRegs();
    }

    [[nodiscard]] const std::array<std::uint32_t, 64>&
    extension_registers() const {
        return jit_->ExtRegs();
    }

    [[nodiscard]] std::uint32_t cpsr() const {
        return jit_->Cpsr();
    }

    void set_cpsr(std::uint32_t value) {
        jit_->SetCpsr(value);
    }

    [[nodiscard]] std::uint32_t fpscr() const {
        return jit_->Fpscr();
    }

    void set_fpscr(std::uint32_t value) {
        jit_->SetFpscr(value);
    }

private:
    [[nodiscard]] static constexpr Dynarmic::HaltReason all_halt_reasons() {
        return Dynarmic::HaltReason::MemoryAbort |
               Dynarmic::HaltReason::UserDefined1 |
               Dynarmic::HaltReason::UserDefined2 |
               Dynarmic::HaltReason::UserDefined3 |
               Dynarmic::HaltReason::UserDefined4 |
               Dynarmic::HaltReason::UserDefined5 |
               Dynarmic::HaltReason::UserDefined6 |
               Dynarmic::HaltReason::UserDefined7 |
               Dynarmic::HaltReason::UserDefined8;
    }

    void ensure_jit() {
        if (jit_) {
            return;
        }
        Dynarmic::A32::UserConfig config{callbacks_.get()};
        config.processor_id = processor_id_;
        config.global_monitor = &monitor_;
        config.arch_version = dynarmic_architecture_version(
            callbacks_->cpu_model().architecture_version());
        config.always_little_endian = true;
        config.enable_cycle_counting = true;
        config.check_halt_on_memory_access = true;
        config.code_cache_size = code_cache_size_;
        using DynarmicPageTable = std::array<
            std::uint8_t*,
            Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;
        static_assert(
            AddressSpace::page_count ==
            Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES);
        auto** read_table = callbacks_->jit_read_page_table();
        auto** write_table = callbacks_->jit_write_page_table();
        if (read_table || write_table) {
            config.read_page_table =
                reinterpret_cast<DynarmicPageTable*>(read_table);
            config.page_table =
                reinterpret_cast<DynarmicPageTable*>(write_table);
            config.detect_misaligned_access_via_page_table =
                static_cast<std::uint8_t>(8U | 16U | 32U | 64U);
            config.only_detect_misalignment_via_page_table_on_page_boundary =
                true;
        }
        const auto measure = performance_counters().enabled();
        const auto started = measure
                                 ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
        jit_ = std::make_unique<Dynarmic::A32::Jit>(config);
        const auto elapsed =
            measure
                ? static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count())
                : 0;
        performance_counters().record_jit(elapsed);
        performance_counters().record_latency(
            PerfLatencyKind::JitColdPath, elapsed);
        record_code_cache_usage();
    }

    void load_state(Cpu& cpu) {
        clear_halt();
        jit_->Regs() = cpu.state_.registers;
        jit_->ExtRegs() = cpu.state_.extension_registers;
        jit_->SetCpsr(cpu.state_.cpsr);
        jit_->SetFpscr(cpu.state_.fpscr);
        cpu.active_executor_ = this;
        callbacks_->attach(&cpu, jit_.get());
        if (static_cast<std::uint32_t>(cpu.requested_halt_reason_) != 0) {
            jit_->HaltExecution(cpu.requested_halt_reason_);
        }
    }

    void save_state(Cpu& cpu) {
        cpu.state_.registers = jit_->Regs();
        cpu.state_.extension_registers = jit_->ExtRegs();
        cpu.state_.cpsr = jit_->Cpsr();
        cpu.state_.fpscr = jit_->Fpscr();
        cpu.active_executor_ = nullptr;
    }

    void record_code_cache_usage() {
        if (!jit_ || !performance_counters().enabled()) {
            return;
        }
        const auto current = jit_code_cache_used(*jit_);
        performance_counters().record_jit_code_cache_usage(
            process_id_, static_cast<std::uint32_t>(execution_slot_),
            recorded_jit_code_cache_bytes_, current);
        recorded_jit_code_cache_bytes_ = current;
    }

  public:
    void set_process_id(std::uint32_t process_id) {
        process_id_ = process_id;
    }

    void set_code_cache_size(std::size_t bytes) {
        std::lock_guard lock{execution_mutex_};
        if (jit_) {
            throw std::logic_error{
                "cannot resize a live Dynarmic code cache"};
        }
        code_cache_size_ = bytes;
    }

  private:
    std::size_t processor_id_{};
    std::size_t execution_slot_{};
    std::uint32_t process_id_{};
    AddressSpace& memory_;
    Dynarmic::ExclusiveMonitor& monitor_;
    std::unique_ptr<JitCallbacks> callbacks_;
    std::unique_ptr<Dynarmic::A32::Jit> jit_;
    std::size_t code_cache_size_{64U * 1024U * 1024U};
    std::uint64_t recorded_jit_code_cache_bytes_{};
    std::deque<std::uint64_t> pending_precompile_entries_;
    std::unordered_set<std::uint64_t> seen_precompile_entries_;
    std::mutex precompile_queue_mutex_;
    std::mutex execution_mutex_;
};

class CpuExecutionPool {
public:
    CpuExecutionPool(
        AddressSpace& memory,
        Dynarmic::ExclusiveMonitor& monitor,
        std::size_t execution_slot_count,
        std::size_t first_processor_id,
        const ArmCpuModel& cpu_model)
        : memory_{memory} {
        if (execution_slot_count == 0) {
            throw std::invalid_argument{
                "execution_slot_count must be at least one"};
        }
        executors_.reserve(execution_slot_count);
        for (std::size_t slot = 0; slot < execution_slot_count; ++slot) {
            executors_.push_back(std::make_unique<JitExecutor>(
                first_processor_id + slot, slot, memory, monitor, cpu_model));
        }
    }

    [[nodiscard]] std::size_t size() const {
        return executors_.size();
    }

    [[nodiscard]] JitExecutor& executor(std::size_t slot) {
        return *executors_.at(slot);
    }

    void set_process_id(std::uint32_t process_id) {
        for (auto& executor : executors_)
            executor->set_process_id(process_id);
    }

    void set_code_cache_size(std::size_t bytes) {
        for (auto& executor : executors_)
            executor->set_code_cache_size(bytes);
    }

    void clear_cache() {
        for (auto& executor : executors_) {
            executor->clear_cache();
        }
    }

    void invalidate_cache_range(std::uint32_t address, std::size_t length) {
        for (auto& executor : executors_) {
            executor->invalidate_cache_range(address, length);
        }
    }

    void disable_jit_page_table() {
        memory_.disable_jit_page_table();
    }

    void set_translation_profile(
        std::shared_ptr<JitTranslationProfile> profile) {
        for (auto& executor : executors_) {
            executor->set_translation_profile(profile);
        }
    }

    std::size_t precompile_pending(
        std::size_t maximum_blocks, std::uint64_t budget_nanoseconds) {
        if (executors_.empty()) {
            return 0;
        }
        const auto blocks_per_executor =
            std::max<std::size_t>(1, maximum_blocks / executors_.size());
        const auto budget_per_executor =
            std::max<std::uint64_t>(1, budget_nanoseconds / executors_.size());
        std::size_t compiled = 0;
        for (auto& executor : executors_) {
            compiled += executor->precompile_pending(
                blocks_per_executor, budget_per_executor);
        }
        return compiled;
    }

private:
    AddressSpace& memory_;
    std::vector<std::unique_ptr<JitExecutor>> executors_;
};

Cpu::Cpu(
    std::size_t processor_id, AddressSpace& memory, Dynarmic::ExclusiveMonitor& monitor)
    : Cpu{processor_id,
          std::make_shared<CpuExecutionPool>(
              memory, monitor, 1, processor_id, default_arm_cpu_model())} {}

Cpu::Cpu(
    std::size_t processor_id,
    std::shared_ptr<CpuExecutionPool> execution_pool)
    : processor_id_{processor_id},
      execution_pool_{std::move(execution_pool)} {}

Cpu::~Cpu() = default;

CpuRunResult Cpu::run(std::uint64_t ticks, std::size_t execution_slot) {
    if (!execution_pool_) {
        throw std::logic_error{"CPU execution resources have been released"};
    }
    return execution_pool_->executor(execution_slot).run(*this, ticks, false);
}

CpuRunResult Cpu::step(std::size_t execution_slot) {
    if (!execution_pool_) {
        throw std::logic_error{"CPU execution resources have been released"};
    }
    return execution_pool_->executor(execution_slot).run(*this, 1, true);
}

void Cpu::reset() {
    state_ = {};
    if (active_executor_) {
        active_executor_->reset_live_state();
    }
}
void Cpu::clear_cache() {
    if (execution_pool_) {
        execution_pool_->clear_cache();
    }
}
void Cpu::invalidate_cache_range(std::uint32_t address, std::size_t length) {
    if (execution_pool_) {
        execution_pool_->invalidate_cache_range(address, length);
    }
}
void Cpu::clear_halt() {
    requested_halt_reason_ = {};
    if (active_executor_) {
        active_executor_->clear_halt();
    }
}
void Cpu::halt(Dynarmic::HaltReason reason) {
    requested_halt_reason_ = requested_halt_reason_ | reason;
    if (active_executor_) {
        active_executor_->halt(reason);
    }
}

Dynarmic::HaltReason Cpu::consume_requested_halt_reason() {
    const auto reason = requested_halt_reason_;
    requested_halt_reason_ = {};
    return reason;
}

std::array<std::uint32_t, 16>& Cpu::registers() {
    return active_executor_ ? active_executor_->registers()
                            : state_.registers;
}
const std::array<std::uint32_t, 16>& Cpu::registers() const {
    return active_executor_ ? active_executor_->registers()
                            : state_.registers;
}
std::uint32_t Cpu::cpsr() const {
    return active_executor_ ? active_executor_->cpsr() : state_.cpsr;
}
void Cpu::set_cpsr(std::uint32_t value) {
    if (active_executor_) {
        active_executor_->set_cpsr(value);
    } else {
        state_.cpsr = value;
    }
}
std::array<std::uint32_t, 64>& Cpu::extension_registers() {
    return active_executor_ ? active_executor_->extension_registers()
                            : state_.extension_registers;
}
const std::array<std::uint32_t, 64>& Cpu::extension_registers() const {
    return active_executor_ ? active_executor_->extension_registers()
                            : state_.extension_registers;
}
std::uint32_t Cpu::fpscr() const {
    return active_executor_ ? active_executor_->fpscr() : state_.fpscr;
}
void Cpu::set_fpscr(std::uint32_t value) {
    if (active_executor_) {
        active_executor_->set_fpscr(value);
    } else {
        state_.fpscr = value;
    }
}
std::optional<std::uint32_t> Cpu::cthread_self() const {
    return state_.cthread_self;
}
void Cpu::set_cthread_self(std::optional<std::uint32_t> value) {
    state_.cthread_self = value;
}
void Cpu::set_svc_handler(SvcHandler handler) {
    svc_handler_ = std::move(handler);
}
void Cpu::set_svc_dispatch_mode(SvcDispatchMode mode) {
    svc_dispatch_mode_ = mode;
}
void Cpu::set_memory_write_watchpoint(
    std::uint32_t address, MemoryWriteHandler handler) {
    if (handler && execution_pool_) {
        execution_pool_->disable_jit_page_table();
    }
    memory_write_watch_address_ = address;
    memory_write_handler_ = std::move(handler);
}
void Cpu::set_debug_breakpoints_enabled(bool enabled) {
    debug_breakpoints_enabled_ = enabled;
}
void Cpu::set_translation_profile(
    std::shared_ptr<JitTranslationProfile> profile) {
    if (execution_pool_) {
        execution_pool_->set_translation_profile(std::move(profile));
    }
}
void Cpu::clear_exclusive_state(std::size_t execution_slot) {
    if (!execution_pool_) {
        return;
    }
    // The local state gates STREX. The next LDREX overwrites this serialized
    // processor's single global slot, so clearing the local state is enough
    // and avoids taking the global monitor lock on every context switch.
    execution_pool_->executor(execution_slot).clear_exclusive_state();
}

CpuCluster::CpuCluster(std::size_t processor_count, AddressSpace& memory)
    : CpuCluster{processor_count, processor_count, memory} {}

CpuCluster::CpuCluster(
    std::size_t initial_processor_count,
    std::size_t maximum_processor_count,
    AddressSpace& memory)
    : CpuCluster{
          initial_processor_count, maximum_processor_count, memory, false} {}

CpuCluster::CpuCluster(
    std::size_t initial_processor_count,
    std::size_t maximum_processor_count,
    AddressSpace& memory,
    bool serialized_execution)
    : CpuCluster{
          initial_processor_count, maximum_processor_count, memory,
          serialized_execution, default_arm_cpu_model()} {}

CpuCluster::CpuCluster(
    std::size_t initial_processor_count,
    std::size_t maximum_processor_count,
    AddressSpace& memory,
    bool serialized_execution,
    const ArmCpuModel& cpu_model)
    : CpuCluster{
          initial_processor_count, maximum_processor_count, memory,
          serialized_execution ? 1U : maximum_processor_count, cpu_model} {}

CpuCluster::CpuCluster(
    std::size_t initial_processor_count,
    std::size_t maximum_processor_count,
    AddressSpace& memory,
    std::size_t execution_slot_count,
    const ArmCpuModel& cpu_model)
    : memory_{&memory},
      maximum_processor_count_{maximum_processor_count},
      serialized_execution_{execution_slot_count == 1},
      cpu_model_{&cpu_model},
      monitor_{execution_slot_count == 0 ? 1U : execution_slot_count},
      execution_pool_{std::make_shared<CpuExecutionPool>(
          memory, monitor_, execution_slot_count, 0, cpu_model)} {
    if (initial_processor_count == 0) {
        throw std::invalid_argument{
            "initial_processor_count must be at least one"};
    }
    if (maximum_processor_count < initial_processor_count) {
        throw std::invalid_argument{
            "maximum_processor_count must cover the initial processors"};
    }
    cpus_.reserve(maximum_processor_count);
    while (cpus_.size() < initial_processor_count) {
        static_cast<void>(add_cpu());
    }
}

std::optional<std::size_t> CpuCluster::add_cpu() {
    if (cpus_.size() >= capacity()) {
        return std::nullopt;
    }
    const auto id = cpus_.size();
    cpus_.push_back(
        std::unique_ptr<Cpu>{new Cpu{id, execution_pool_}});
    return id;
}

void CpuCluster::set_process_id(std::uint32_t process_id) {
    execution_pool_->set_process_id(process_id);
}

void CpuCluster::set_jit_code_cache_size(std::size_t bytes) {
    execution_pool_->set_code_cache_size(bytes);
}

void CpuCluster::clear_cache() {
    execution_pool_->clear_cache();
}

void CpuCluster::invalidate_cache_range(
    std::uint32_t address, std::size_t length) {
    execution_pool_->invalidate_cache_range(address, length);
}

void CpuCluster::set_translation_profile(
    std::shared_ptr<JitTranslationProfile> profile) {
    execution_pool_->set_translation_profile(std::move(profile));
}

std::size_t CpuCluster::precompile_pending(
    std::size_t maximum_blocks, std::uint64_t budget_nanoseconds) {
    if (!execution_pool_) {
        return 0;
    }
    return execution_pool_->precompile_pending(
        maximum_blocks, budget_nanoseconds);
}

std::shared_ptr<CpuExecutionPool>
CpuCluster::release_execution_resources() {
    if (!execution_pool_) {
        return {};
    }
    for (const auto& cpu : cpus_) {
        if (cpu->active_executor_ != nullptr) {
            throw std::logic_error{
                "cannot release CPU execution resources while executing"};
        }
    }
    auto retired = std::move(execution_pool_);
    for (auto& cpu : cpus_) {
        cpu->execution_pool_.reset();
    }
    return retired;
}

std::vector<CpuRunResult> CpuCluster::run_parallel(std::uint64_t ticks_per_cpu) {
    if (!execution_pool_) {
        throw std::logic_error{
            "CPU execution resources have been released"};
    }
    if (serialized_execution_ && cpus_.size() > 1) {
        throw std::logic_error{
            "serialized CPU contexts cannot execute in parallel"};
    }
    std::vector<CpuRunResult> results(cpus_.size());
    std::vector<std::thread> workers;
    workers.reserve(cpus_.size());
    for (std::size_t index = 0; index < cpus_.size(); ++index) {
        workers.emplace_back([&, index] {
            results[index] = cpus_[index]->run(ticks_per_cpu, index);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    return results;
}

}  // namespace ilemu
