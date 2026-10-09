/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/run.hpp"

namespace ilemu::execution::arm_memory {
// A decoded memory transaction can cross the native/C++ boundary without
// retaining decoder objects, memory pointers or host ABI-dependent containers.
struct Transfer {
    std::uint64_t ticks = 0;
    std::uint32_t address = 0, updated_base = 0, value = 0, control = 0;
};
constexpr std::uint32_t load_bit = 1U << 3;
constexpr std::uint32_t signed_bit = 1U << 4;
constexpr std::uint32_t writeback_bit = 1U << 5;
constexpr std::uint32_t control(const arm::Instruction& inst)
{
    return inst.access_size | (inst.load ? load_bit : 0U) |
           (inst.sign_extend ? signed_bit : 0U) |
           (inst.writeback ? writeback_bit : 0U) | (inst.rd << 8U) |
           (inst.rn << 12U) | (inst.size << 16U);
}
Transfer prepare(const CpuThreadState&, const arm::Instruction&,
    std::uint32_t pc_store_offset);
struct Completion {
    StopReason reason = StopReason::None;
    std::optional<MemoryFault> fault;
};
// Commit destination/base/PC only after successful access. A data abort records
// the adapter's fault registers without retiring the faulting instruction.
Completion complete(CpuThreadState&, MemoryAccess&, const Transfer&);
}
