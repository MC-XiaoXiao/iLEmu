/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm_memory/transfer.hpp"

namespace ilemu::execution::arm_memory {
// One architectural instruction; checked accesses may complete a prefix before
// an abort. The original base allows restart without repeating writeback.
struct MultipleTransfer {
    std::uint64_t ticks = 0;
    std::uint32_t address = 0, base = 0, updated_base = 0;
    std::uint32_t pc_value = 0, control = 0;
};
constexpr std::uint32_t multiple_load = 1U << 16;
constexpr std::uint32_t multiple_writeback = 1U << 17;
constexpr std::uint32_t multiple_control(const arm::Instruction& inst)
{
    return inst.registers | (inst.load ? multiple_load : 0U) |
           (inst.writeback ? multiple_writeback : 0U) | (inst.rn << 20U) |
           (inst.size << 24U);
}
MultipleTransfer prepare_multiple(const CpuThreadState&,
    const arm::Instruction&, std::uint32_t pc_store_offset);
Completion complete_multiple(
    CpuThreadState&, MemoryAccess&, const MultipleTransfer&);
}
