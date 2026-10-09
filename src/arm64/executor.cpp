/* SPDX-License-Identifier: MPL-2.0 */
#include "arm64/executor.hpp"
#include "arm/t32_decode.hpp"
#include "compiler.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ilemu::execution {
class Arm64Executor::Impl {
public:
    struct Entry {
        std::unique_ptr<ExecutableCode> code;
        std::uint64_t maximum_ticks;
        std::uint64_t timing_limit;
        unsigned instructions;
        bool closed;
        bool accesses_memory;
        std::optional<std::uint32_t> first_instruction;
        std::size_t accounted_bytes;
    };
    Impl(CodeAllocator& allocator, std::size_t budget)
        : allocator_(allocator)
        , budget_(budget)
    {
        if (budget < 256U * 1024U)
            throw std::invalid_argument(
                "ARM64 code cache requires at least 256 KiB");
    }
    void clear()
    {
        cache_.clear();
        stats_.retained_bytes = 0;
    }
    Entry& entry(InstructionSource& source, std::uint32_t pc, bool step,
        bool big_endian, bool thumb, unsigned it_state,
        std::uint64_t timing_limit = UINT64_MAX)
    {
        const auto key = std::tuple { pc, step, big_endian, thumb, it_state };
        if (const auto it = cache_.find(key);
            it != cache_.end() && it->second.timing_limit == timing_limit) {
            ++stats_.cache_hits;
            return it->second;
        }
        const auto started = std::chrono::steady_clock::now();
        const auto compiled = arm64::compile(
            source, pc, step, timing_limit, big_endian, thumb, it_state);
        const auto bytes = std::as_bytes(std::span { compiled.words });
        if (bytes.size() > 256U * 1024U)
            throw std::length_error("ARM64 trace exceeds code limit");
        auto allocation = allocator_.allocate(bytes.size());
        if (!allocation)
            throw std::bad_alloc { };
        constexpr auto metadata_bytes =
            sizeof(Entry) + 64U; // bounded map node/key overhead
        const auto capacity = allocation->capacity();
        if (capacity > budget_ - metadata_bytes)
            throw std::length_error("ARM64 trace exceeds cache budget");
        const auto accounted = capacity + metadata_bytes;
        // The owner is between native calls. No active entry can be reclaimed.
        if (stats_.retained_bytes > budget_ - accounted || cache_.contains(key))
            clear();
        allocation->publish(bytes);
        if (!allocation->entry())
            throw std::runtime_error("ARM64 code was not published");
        ++stats_.compiled_regions;
        stats_.emitted_bytes += bytes.size();
        auto [it, inserted] = cache_.emplace(
            key, Entry { std::move(allocation), compiled.maximum_ticks,
                     timing_limit, compiled.instructions, compiled.closed,
                     compiled.accesses_memory, compiled.first_instruction,
                     accounted });
        if (!inserted)
            throw std::logic_error("duplicate ARM64 cache entry");
        stats_.retained_bytes += accounted;
        stats_.compilation_nanoseconds += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
        return it->second;
    }
    RunResult run(CpuThreadState& state, InstructionSource& source,
        const RunRequest& request)
    {
        if (!Arm64Executor::available())
            throw std::runtime_error("ARM64 executor requires an AArch64 host");
        // No source pointer, address-space identity or lease survives a Run
        // binding. A later cache-sharing policy can provide stable identities.
        clear();
        std::optional<std::uint64_t> generation;
        RunResult result;
        for (;;) {
            result.pc = state.registers[15];
            result.instruction.reset();
            result.memory_fault.reset();
            result.reason = stops_.consume();
            const bool step = request.mode == ExecutionMode::SingleStep;
            if (!step && result.ticks_consumed >= request.tick_budget)
                result.reason = result.reason | StopReason::TickBudget;
            if (request.deadline !=
                    std::chrono::steady_clock::time_point::max() &&
                std::chrono::steady_clock::now() >= request.deadline)
                result.reason = result.reason | StopReason::HostDeadline;
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
            auto lease = source.acquire_code_lease();
            if (!lease)
                throw std::runtime_error(
                    "ARM64 executor requires a stable instruction lease");
            const auto current = lease->generation();
            if (!current || current != generation) {
                clear();
                generation = current;
            }
            const bool big_endian = (state.cpsr & (1U << 9U)) != 0;
            auto* selected =
                &entry(source, result.pc, step, big_endian, thumb, it_state);
            const auto remaining =
                step ? UINT64_MAX : request.tick_budget - result.ticks_consumed;
            // Short budget tails execute a native one-instruction entry. This
            // preserves the interpreter's budget boundary without per-op polls.
            // Equality also prevents observing a fault immediately after the
            // final paid instruction, before the next budget check.
            if (!step && selected->maximum_ticks >= remaining)
                selected = &entry(
                    source, result.pc, true, big_endian, thumb, it_state);
            const auto allowance = UINT64_MAX - result.ticks_consumed;
            if (selected->maximum_ticks > allowance)
                selected = &entry(source, result.pc, true, big_endian, thumb,
                    it_state, allowance);
            arm64::NativeOutcome native;
            auto* memory = source.data_memory();
            if (memory && selected->accesses_memory)
                native.memory = memory->direct_memory();
            if (selected->closed && selected->maximum_ticks != 0) {
                native.groups = std::min<std::uint64_t>(
                    1024, remaining / selected->maximum_ticks);
                if (request.deadline !=
                    std::chrono::steady_clock::time_point::max())
                    native.groups = std::min<std::uint64_t>(native.groups,
                        std::max(1U, 4096U / selected->instructions));
                native.groups = std::max<std::uint64_t>(1, native.groups);
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
            if (native.reason == arm64::memory_exit) {
                if (!memory)
                    throw std::logic_error("memory exit has no bound memory");
                const bool memory_thumb = (state.cpsr & 0x20U) != 0;
                const auto memory_it = arm::it_state(state.cpsr);
                const auto completion =
                    arm_memory::complete(state, *memory, native.transfer);
                native.reason = static_cast<std::uint32_t>(completion.reason);
                result.memory_fault = completion.fault;
                if (completion.reason == StopReason::None) {
                    if (memory_thumb)
                        state.cpsr = arm::with_it_state(
                            state.cpsr, arm::advance_it(memory_it));
                    native.ticks += native.transfer.ticks;
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
                            has_reason(result.reason, StopReason::Svc))) {
                result.reason = result.reason | StopReason::SingleStep;
                if (!result.instruction)
                    result.instruction = selected->first_instruction;
            }
            if (result.reason != StopReason::None)
                return result;
        }
    }
    CodeAllocator& allocator_;
    std::size_t budget_;
    StopRequests stops_;
    Arm64Statistics stats_;
    std::map<std::tuple<std::uint32_t, bool, bool, bool, unsigned>, Entry>
        cache_;
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
void Arm64Executor::clear_cache() { impl_->clear(); }
Arm64Statistics Arm64Executor::statistics() const noexcept
{
    return impl_->stats_;
}
}
