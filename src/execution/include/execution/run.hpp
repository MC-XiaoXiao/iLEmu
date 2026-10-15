/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "execution/arm_state.hpp"
#include "execution/instruction_profile.hpp"
#include "execution/memory.hpp"

#include <array>
#include <compare>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace ilemu::execution {

enum class StopReason : std::uint32_t {
    None = 0,
    TickBudget = 1U << 0,
    SingleStep = 1U << 1,
    HostDeadline = 1U << 2,
    GuestPreemption = 1U << 3,
    Cancelled = 1U << 4,
    Svc = 1U << 5,
    FetchFault = 1U << 6,
    UnsupportedInstruction = 1U << 7,
    InvalidTiming = 1U << 8,
    DataFault = 1U << 9,
    Breakpoint = 1U << 10,
};
constexpr StopReason operator|(StopReason a, StopReason b) noexcept
{
    return static_cast<StopReason>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr bool has_reason(StopReason value, StopReason flag) noexcept
{
    return (static_cast<std::uint32_t>(value) &
               static_cast<std::uint32_t>(flag)) != 0;
}

enum class ExecutionMode { Run, SingleStep };
// Optional, run-scoped diagnostics on the execution owner. Called once after
// each successfully published translation, including code-free stop plans.
// Observers must not reenter execution or access guest state/memory. Their
// cost is excluded from the reported translation duration.
class TranslationObserver {
public:
    virtual ~TranslationObserver() = default;
    virtual void translation_completed(std::uint64_t nanoseconds) noexcept = 0;
};
struct RunRequest {
    std::uint64_t tick_budget;
    ExecutionMode mode = ExecutionMode::Run;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max();
    TranslationObserver* translation_observer = nullptr;
};
struct RunResult {
    StopReason reason = StopReason::None;
    std::uint64_t ticks_consumed = 0;
    std::optional<std::uint32_t> svc;
    // Fault/unsupported/SVC report their instruction PC; normal exits report
    // the next unexecuted instruction. Unsupported does not retire a step.
    std::uint32_t pc = 0;
    std::optional<std::uint32_t> instruction;
    std::optional<MemoryFault> memory_fault;
};

// Opaque instruction-view identity. Tokens must not own guest backing or source
// objects. Rotate the token if generation values can restart or be reused.
struct CodeIdentity final { };

// Opt-in cross-source translation provenance. Equal identities must describe
// identical instruction bytes, fetch support and timing for the entire leased
// region (including a straddling entry). Tokens own no guest storage or source.
// Each physical storage id is globally unique and never reused; writes and COW
// must change its stamp. The semantic token names immutable timing policy.
struct CompilationIdentity {
    struct PageStamp {
        std::uint64_t storage = 0, write = 0, shared_write = 0;
        auto operator<=>(const PageStamp&) const = default;
    };
    std::shared_ptr<const CodeIdentity> semantics;
    std::array<PageStamp, 2> pages;
    unsigned page_count = 0;
    auto operator<=>(const CompilationIdentity&) const = default;
};

struct InstructionRegion {
    std::uint64_t begin = 0;
    std::uint64_t end = std::uint64_t { 1 } << 32U;
};

// A code view validated and pinned by a live lease. This is an opaque token,
// not ownership of guest bytes; all validity ends with the enclosing lease.
struct LeasedCodeView {
    std::shared_ptr<const CodeIdentity> identity;
    std::optional<std::uint64_t> generation;
    InstructionRegion region;
    // Opt-in inspection contract, separate from executing cached native code.
    // fetch16/fetch32 and timing can inspect this entire region under the
    // current lease without preparing a fetch or charging guest VM events.
    bool inspectable = false;
};

// A lease excludes external code writes and mapping/protection changes.
// Owner checked accesses may change code/mappings, but must end native
// execution before doing so and reacquire the lease before any subsequent
// instruction. A generation, when supplied, also covers instruction timing and
// all shared aliases; otherwise compiled code must be revalidated at every
// lease entry. An optional identity plus generation permits reuse across Run
// calls. It identifies bytes, executable permissions, fetch support and timing,
// including shared aliases; equal generations must mean equal instruction
// views.
class InstructionLease {
public:
    virtual ~InstructionLease() = default;
    // Optional cache-only continuation lookup. Never prepares a fetch, charges
    // guest VM events, materializes memory, or reports a speculative fault.
    // Success pins the complete region and validates its current view until
    // lease release. Contention or an unavailable view returns empty promptly.
    virtual std::optional<LeasedCodeView>
    cached_continuation(std::uint32_t) noexcept { return {}; }
    // Prepared executable bytes, permissions, timing and mappings throughout
    // this region stay valid until release, including successor entry fetches.
    // Cached native entries may continue here without preparing another fetch.
    // Sources with entry-specific side effects must retain the default.
    virtual std::optional<InstructionRegion>
    continuation_region() const noexcept { return {}; }
    virtual std::shared_ptr<const CodeIdentity> identity() const noexcept
    {
        return { };
    }
    // Unknown/custom sources remain local unless they explicitly supply this
    // stronger contract. Query only after all entry fetches have been prepared.
    virtual std::optional<CompilationIdentity>
    compilation_identity() const noexcept { return { }; }
    virtual std::optional<std::uint64_t> generation() const noexcept
    {
        return { };
    }
};

// Bound at Run entry; a source owns permissions, backing lifetime and timing.
// No pointer into guest bytes or backend descriptor escapes this interface.
class InstructionSource {
public:
    virtual ~InstructionSource() = default;
    virtual std::unique_ptr<InstructionLease> acquire_code_lease()
    {
        return { };
    }
    // A runtime source can lease just the entry region and any straddling
    // instruction. The legacy overload remains available to whole-view sources.
    virtual std::unique_ptr<InstructionLease> acquire_code_lease(
        std::uint32_t, bool)
    {
        return acquire_code_lease();
    }
    // Fixed throughout a Run. Native cache keys include the profile, so a
    // source can change CPU policy between calls without reusing stale code.
    virtual ThumbArithmeticProfile thumb_arithmetic_profile() const noexcept
    {
        return ThumbArithmeticProfile::Architectural;
    }
    virtual ThumbMemoryProfile thumb_memory_profile() const noexcept
    {
        return ThumbMemoryProfile::Architectural;
    }
    virtual AdvancedSimdProfile advanced_simd_profile() const noexcept
    {
        return AdvancedSimdProfile::None;
    }
    virtual VfpRegisterProfile vfp_register_profile() const noexcept
    {
        return VfpRegisterProfile::D32;
    }
    virtual VfpInstructionProfile vfp_instruction_profile() const noexcept
    {
        return VfpInstructionProfile::Vfpv3;
    }
    virtual ArmInstructionProfile arm_instruction_profile() const noexcept
    {
        return ArmInstructionProfile::Armv6T2;
    }
    virtual IntegerDivideProfile integer_divide_profile() const noexcept
    {
        return IntegerDivideProfile::None;
    }
    virtual AluPcWriteProfile alu_pc_write_profile() const noexcept
    {
        return AluPcWriteProfile::Interworking;
    }
    virtual FloatingExceptionProfile floating_exception_profile() const noexcept
    {
        return FloatingExceptionProfile::Untrapped;
    }
    virtual MemoryAccess* data_memory() noexcept { return nullptr; }
    // Executed fetches install/validate runtime translations and charge guest
    // VM events. Inspection below must not charge them. Thumb-wide fetches
    // prepare each halfword separately, including a second-page fault. Empty
    // means success; failures retain the actual fetch address/status in RunResult.
    virtual std::optional<MemoryFault> prepare_instruction_fetch(
        std::uint32_t, unsigned)
    {
        return { };
    }
    // Native traces remain within this region, except a first instruction
    // straddling its end, which executes alone. Runtime sources can use VM
    // pages so an unexecuted successor never receives a prepared fetch. Within
    // a region, a prepared entry authorizes later instructions for the lease;
    // sources with narrower runtime fetch checks must use narrower regions.
    // Bounds and preparation policy belong to the leased instruction view.
    virtual InstructionRegion instruction_region(std::uint32_t) const
    {
        return { };
    }
    // Thumb fetches must not require the neighbouring halfword to be mapped
    // executable. Sources without halfword support reject Thumb explicitly.
    virtual std::optional<std::uint16_t> fetch16(std::uint32_t) { return { }; }
    virtual std::optional<std::uint32_t> fetch32(std::uint32_t address) = 0;
    virtual std::uint64_t ticks_for_instruction(
        std::uint32_t, std::uint32_t) const
    {
        return 1;
    }
    // Fixed throughout a Run, and part of native compilation identity.
    // Sources whose timing depends on the entry lease can keep the default.
    virtual bool supports_mixed_instruction_timing() const noexcept
    {
        return false;
    }
    virtual std::uint64_t ticks_for_instruction(
        bool, std::uint32_t pc, std::uint32_t word) const
    {
        return ticks_for_instruction(pc, word);
    }
};

// A backend binds architectural context and memory only for this call. Runtime
// slots may own implementations without embedding process or host UI state.
class Executor {
public:
    virtual ~Executor() = default;
    virtual RunResult run(
        CpuThreadState&, InstructionSource&, const RunRequest&) = 0;
    virtual void request_stop(StopReason) noexcept = 0;
    virtual void clear_stop() noexcept = 0;
    // Owner-only, outside a backend invocation. Interpreters have no cache.
    virtual void clear_cache() { }
    // Sources can inspect their entire instruction region during translation.
    // Backends without dependency tracking retain conservative invalidation.
    virtual void invalidate_cache_range(std::uint32_t, std::size_t size)
    {
        if (size != 0)
            clear_cache();
    }
    // Real architectural context switches, not cooperative slices/cache work.
    virtual void clear_exclusive_state() { }
    virtual std::uint64_t retained_code_bytes() const noexcept { return 0; }
    virtual std::uint64_t translation_count() const noexcept { return 0; }
};

// Only the execution owner accesses guest state. Other threads request a stop.
class StopRequests {
public:
    void request(StopReason reason) noexcept
    {
        pending_.fetch_or(
            static_cast<std::uint32_t>(reason), std::memory_order_release);
    }
    StopReason consume() noexcept
    {
        // An empty poll linearizes at the load; a later request remains
        // pending for the next poll without an unnecessary atomic write.
        if (pending_.load(std::memory_order_acquire) == 0)
            return StopReason::None;
        return static_cast<StopReason>(
            pending_.exchange(0, std::memory_order_acq_rel));
    }

private:
    std::atomic<std::uint32_t> pending_ { 0 };
};

}
