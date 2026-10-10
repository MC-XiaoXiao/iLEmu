/* SPDX-License-Identifier: MPL-2.0 */
#include "arm_memory/exclusive.hpp"
#include "endian.hpp"
namespace ilemu::execution::arm_memory {
ExclusiveTransfer prepare_exclusive(const CpuThreadState& state,
    const arm::Instruction& inst)
{
    std::uint64_t value = 0;
    if (!inst.load && inst.kind != arm::InstructionKind::ClearExclusive) {
        value = state.registers[inst.rm];
        if (inst.access_size == 8)
            value |= std::uint64_t { state.registers[inst.rs] } << 32U;
    }
    return { 0, state.registers[inst.rn] + inst.immediate,
        exclusive_control(inst), value };
}
Completion complete_exclusive(CpuThreadState& state, MemoryAccess* memory,
    std::unique_ptr<ExclusiveMonitor>& monitor, const ExclusiveTransfer& transfer)
{
    if ((transfer.control & exclusive_clear) != 0) {
        if (monitor)
            monitor->clear();
    } else {
        if (!memory)
            return { StopReason::UnsupportedInstruction, { } };
        if (!monitor || !monitor->bound_to(*memory))
            monitor = memory->create_exclusive_monitor();
        if (!monitor)
            return { StopReason::UnsupportedInstruction, { } };
        const auto size = transfer.control & 15U;
        const auto rd = (transfer.control >> 8U) & 15U;
        const auto second = (transfer.control >> 12U) & 15U;
        const bool big = (state.cpsr & 512U) != 0;
        const auto endian = [&](std::uint64_t value) {
            const auto first = static_cast<std::uint32_t>(value);
            if (size == 8)
                return std::uint64_t { reverse(first, 4) } |
                    (std::uint64_t { reverse(static_cast<std::uint32_t>(value >> 32U), 4) } << 32U);
            return std::uint64_t { reverse(first, size) };
        };
        std::optional<MemoryFault> fault;
        if ((transfer.control & exclusive_load) != 0) {
            auto read = monitor->load(transfer.address, size);
            fault = read.fault;
            if (!fault) {
                const auto value = big ? endian(read.value) : read.value;
                const auto mask = size == 1 ? 0xffU : size == 2 ? 0xffffU : UINT32_MAX;
                state.registers[rd] = static_cast<std::uint32_t>(value) & mask;
                if (size == 8)
                    state.registers[second] = static_cast<std::uint32_t>(value >> 32U);
            }
        } else {
            const auto result = monitor->store(transfer.address, size,
                big ? endian(transfer.value) : transfer.value);
            fault = result.fault;
            if (!fault)
                state.registers[rd] = result.stored ? 0U : 1U;
        }
        if (fault) {
            state.abort_state = { fault->status, fault->address };
            return { StopReason::DataFault, fault };
        }
    }
    state.registers[15] += (transfer.control >> 16U) & 7U;
    return { };
}
}
