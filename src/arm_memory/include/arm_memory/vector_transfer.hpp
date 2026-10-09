/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm_memory/transfer.hpp"

namespace ilemu::execution::arm_memory {
struct VectorTransfer {
    std::uint64_t ticks = 0;
    std::uint32_t address = 0, updated_base = 0, control = 0, alignment = 1;
};
constexpr std::uint32_t vector_load = 1U << 16U;
constexpr std::uint32_t vector_writeback = 1U << 17U;
constexpr std::uint32_t vector_control(const arm::Instruction& inst)
{
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    return v.first | (v.count << 5U) | (v.element_size << 8U) |
           (inst.rn << 12U) | (inst.load ? vector_load : 0U) |
           (inst.writeback ? vector_writeback : 0U) | (inst.size << 20U) |
           (static_cast<unsigned>(v.mode) << 23U) | (v.lane << 25U);
}
VectorTransfer prepare_vector(
    const CpuThreadState&, const arm::Instruction&, bool permits_unaligned);
Completion complete_vector(
    CpuThreadState&, MemoryAccess&, const VectorTransfer&);
}
