/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm_memory/multiple.hpp"
#include "arm_memory/transfer.hpp"
#include "arm_memory/exclusive.hpp"
#include "arm_memory/vector_transfer.hpp"
#include "execution/run.hpp"
#include "cycle_tier.hpp"
#include "native_continuations.hpp"
#include "trace_metadata.hpp"
#include "arm/architectural_stop.hpp"
#include <cstdint>
#include <vector>
namespace ilemu::execution::arm64 {
// Plain native ABI; no backend or standard-library object layout is embedded
// in generated instructions, except the shared architectural state offsets.
struct NativeOutcome {
    std::uint64_t groups = 1;
    // Run-scoped, non-throwing control check. No guest state or memory changes
    // are permitted while native registers and the instruction lease are live.
    std::uint32_t (*poll)(void*) noexcept = nullptr;
    void* control = nullptr;
    std::uint64_t ticks = 0;
    std::uint32_t reason = 0;
    std::uint32_t pc = 0;
    std::uint32_t word = 0;
    std::uint32_t svc = 0;
    std::uint32_t has_instruction = 0;
    DirectMemory memory;
    arm_memory::Transfer transfer;
    arm_memory::MultipleTransfer multiple;
    arm_memory::VectorTransfer vector;
    arm_memory::ExclusiveTransfer exclusive;
    std::uint32_t thread_pointer_read_only = 0;
    const void* continuation_dispatch = nullptr;
    NativeContinuation* continuations = nullptr;
    NativeContinuation* active_continuation = nullptr;
    NativeContinuation root_continuation;
    std::uint64_t continuation_ticks = 0;
    std::uint64_t continuation_remaining = 0;
    std::uint32_t continuation_hops = 0;
    std::uint32_t continuation_calls = 0;
    std::uint32_t continuation_stopped = 0;
    std::uint64_t continuation_cookie = 0;
    NativeContinuation* (*resolve_continuation)(
        void*, std::uint32_t, std::uint32_t) noexcept = nullptr;
    void* continuation_context = nullptr;
    std::uint32_t cross_region_calls = 0;
};
inline constexpr std::uint32_t exclusive_exit = 1U << 28U;
inline constexpr std::uint32_t vector_exit = 1U << 29U;
inline constexpr std::uint32_t multiple_exit = 1U << 30U;
inline constexpr std::uint32_t memory_exit = 1U << 31U;
struct CompiledTrace : TraceMetadata {
    std::vector<std::uint32_t> words;
};
CompiledTrace compile(InstructionSource&, std::uint32_t pc, bool single_step,
    std::uint64_t maximum_ticks = UINT64_MAX, bool big_endian = false,
    bool thumb = false, unsigned it = 0,
    CycleExpansion = CycleExpansion::Untracked);
}
