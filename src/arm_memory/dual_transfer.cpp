/* SPDX-License-Identifier: MPL-2.0 */
#include "dual_transfer.hpp"
#include "endian.hpp"

namespace ilemu::execution::arm_memory {
Completion complete_dual(CpuThreadState& state, MemoryAccess& memory,
    const Transfer& transfer)
{
    const bool load = (transfer.control & load_bit) != 0;
    const bool big = (state.cpsr & (1U << 9U)) != 0;
    const auto abort = [&](MemoryFault fault) {
        state.abort_state = { fault.status, fault.address };
        return Completion { StopReason::DataFault, fault };
    };
    if ((transfer.address & 3U) != 0)
        return abort({ transfer.address, 1U | (load ? 0U : 1U << 11U) });
    const unsigned registers[] = { (transfer.control >> 8U) & 15U,
        (transfer.control >> 20U) & 15U };
    const std::uint32_t values[] = { transfer.value, transfer.second_value };
    // The supported ARMv6/ARMv7 memory model performs two aligned words.
    // Preserve a completed prefix on abort, but defer base/PC retirement.
    for (unsigned i = 0; i < 2; ++i) {
        const auto address = transfer.address + i * 4U;
        if (load) {
            const auto read = memory.read(address, AccessSize::Word);
            if (read.fault)
                return abort(*read.fault);
            state.registers[registers[i]] = big ? reverse(read.value, 4) : read.value;
        } else {
            if (const auto fault = memory.write(address, AccessSize::Word,
                    big ? reverse(values[i], 4) : values[i]))
                return abort(*fault);
        }
    }
    if ((transfer.control & writeback_bit) != 0)
        state.registers[(transfer.control >> 12U) & 15U] = transfer.updated_base;
    state.registers[15] += (transfer.control >> 16U) & 7U;
    return { };
}
}
