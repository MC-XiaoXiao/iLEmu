/* SPDX-License-Identifier: MPL-2.0 */
#include "arm_memory/exclusive.hpp"
#include "endian.hpp"
namespace ilemu::execution::arm_memory {
Completion complete_exchange(CpuThreadState& state, MemoryAccess* memory,
    const ExclusiveTransfer& transfer)
{
    if (!memory)
        return { StopReason::UnsupportedInstruction, { } };
    const auto size = transfer.control & 15U;
    const auto abort = [&](MemoryFault fault) {
        state.abort_state = { fault.status, fault.address };
        return Completion { StopReason::DataFault, fault };
    };
    // SWP faults on its read first. The currently represented ARMv6/ARMv7
    // alignment configurations require natural word alignment for exchanges.
    if ((transfer.address & (size - 1U)) != 0)
        return abort({ transfer.address, 1U });
    const bool big = (state.cpsr & 512U) != 0;
    auto value = static_cast<std::uint32_t>(transfer.value);
    const auto result = memory->exchange(transfer.address,
        static_cast<AccessSize>(size), big ? reverse(value, size) : value);
    if (!result)
        return { StopReason::UnsupportedInstruction, { } };
    if (result->fault)
        return abort(*result->fault);
    value = big ? reverse(result->value, size) : result->value;
    state.registers[(transfer.control >> 8U) & 15U] =
        size == 1 ? value & 255U : value;
    return { };
}
}
