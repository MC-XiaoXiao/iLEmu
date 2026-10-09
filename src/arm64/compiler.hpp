/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm_memory/transfer.hpp"
#include "execution/run.hpp"
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
};
inline constexpr std::uint32_t memory_exit = 1U << 31U;
struct CompiledTrace {
    std::vector<std::uint32_t> words;
    std::uint64_t maximum_ticks = 0;
    unsigned instructions = 0;
    bool closed = false;
    bool accesses_memory = false;
    std::optional<std::uint32_t> first_instruction;
};
CompiledTrace compile(InstructionSource&, std::uint32_t pc, bool single_step,
    std::uint64_t maximum_ticks = UINT64_MAX, bool big_endian = false,
    bool thumb = false, unsigned it = 0);
}
