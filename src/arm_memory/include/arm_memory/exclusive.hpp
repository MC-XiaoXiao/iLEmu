/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm_memory/transfer.hpp"
namespace ilemu::execution::arm_memory {
struct ExclusiveTransfer {
    std::uint64_t ticks = 0;
    std::uint32_t address = 0, control = 0;
    std::uint64_t value = 0;
};
inline constexpr std::uint32_t exclusive_load = 1U << 4U;
inline constexpr std::uint32_t exclusive_clear = 1U << 5U;
constexpr std::uint32_t exclusive_control(const arm::Instruction& inst)
{
    return inst.access_size | (inst.load ? exclusive_load : 0U) |
        (inst.kind == arm::InstructionKind::ClearExclusive ? exclusive_clear : 0U) |
        (inst.rd << 8U) | (inst.rs << 12U) | (inst.size << 16U);
}
ExclusiveTransfer prepare_exclusive(const CpuThreadState&, const arm::Instruction&);
Completion complete_exclusive(CpuThreadState&, MemoryAccess*,
    std::unique_ptr<ExclusiveMonitor>&, const ExclusiveTransfer&);
}
