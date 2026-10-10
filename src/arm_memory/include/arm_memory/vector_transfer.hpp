/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm_memory/transfer.hpp"

namespace ilemu::execution::arm_memory {
struct VectorTransfer {
    std::uint64_t ticks = 0;
    std::uint32_t address = 0, updated_base = 0, control = 0, alignment = 1;
};
constexpr std::uint32_t vector_load = 1U << 19U;
constexpr std::uint32_t vector_writeback = 1U << 20U;
constexpr unsigned vector_transfer_length(const arm::VectorTransferOperands& v)
{
    return v.mode == arm::VectorTransferMode::ScalarMultiple
               ? v.count * v.element_size
               : v.mode == arm::VectorTransferMode::Multiple ? v.count * 8U
                                                           : v.element_size;
}
constexpr std::uint32_t vector_control(const arm::Instruction& inst)
{
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    // 5-bit first, 6-bit count (up to32 S registers), 4-bit element size,
    // 4-bit base, load/writeback, 3-bit instruction size, 2-bit mode, 3-bit lane.
    return v.first | (v.count << 5U) | (v.element_size << 11U) |
           (inst.rn << 15U) | (inst.load ? vector_load : 0U) |
           (inst.writeback ? vector_writeback : 0U) | (inst.size << 21U) |
           (static_cast<unsigned>(v.mode) << 24U) | (v.lane << 26U);
}
VectorTransfer prepare_vector(
    const CpuThreadState&, const arm::Instruction&, bool permits_unaligned);
Completion complete_vector(
    CpuThreadState&, MemoryAccess&, const VectorTransfer&);
}
