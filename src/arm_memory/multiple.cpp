/* SPDX-License-Identifier: MPL-2.0 */
#include "arm_memory/multiple.hpp"
#include <bit>

namespace ilemu::execution::arm_memory {
namespace {
    std::uint32_t endian(std::uint32_t value, bool big)
    {
        return big ? ((value & 255U) << 24U) | ((value & 0xff00U) << 8U) |
                         ((value >> 8U) & 0xff00U) | (value >> 24U)
                   : value;
    }
}
MultipleTransfer prepare_multiple(const CpuThreadState& state,
    const arm::Instruction& inst, std::uint32_t pc_store_offset)
{
    const auto base = state.registers[inst.rn];
    const auto length =
        4U * static_cast<unsigned>(std::popcount(inst.registers));
    const auto address = inst.add ? base : base - length;
    return { 0, address + (inst.index == inst.add ? 4U : 0U), base,
        inst.add ? base + length : base - length,
        state.registers[15] + pc_store_offset, multiple_control(inst) };
}
Completion complete_multiple(CpuThreadState& state, MemoryAccess& memory,
    const MultipleTransfer& transfer)
{
    const auto registers = transfer.control & 0xffffU;
    const auto rn = (transfer.control >> 20U) & 15U;
    const bool load = (transfer.control & multiple_load) != 0;
    const bool big = (state.cpsr & (1U << 9U)) != 0;
    const auto pc = state.registers[15];
    const auto abort = [&](MemoryFault fault) {
        // Previously completed accesses remain visible; the faulting
        // instruction and base register remain restartable.
        state.registers[rn] = transfer.base;
        state.abort_state = { fault.status, fault.address };
        return Completion { StopReason::DataFault, fault };
    };
    if ((transfer.address & 3U) != 0)
        return abort({ transfer.address, 1U | (load ? 0U : 1U << 11U) });
    auto address = transfer.address;
    std::uint32_t target = pc;
    for (unsigned reg = 0; reg < 16; ++reg) {
        if ((registers & (1U << reg)) == 0)
            continue;
        if (load) {
            const auto result = memory.read(address, AccessSize::Word);
            if (result.fault)
                return abort(*result.fault);
            const auto value = endian(result.value, big);
            if (reg == 15) {
                if ((value & 3U) == 2U) {
                    state.registers[rn] = transfer.base;
                    return { StopReason::UnsupportedInstruction, { } };
                }
                target = value;
            } else
                state.registers[reg] = value;
        } else {
            const auto value =
                reg == 15 ? transfer.pc_value : state.registers[reg];
            if (const auto fault =
                    memory.write(address, AccessSize::Word, endian(value, big)))
                return abort(*fault);
        }
        address += 4U;
    }
    if ((transfer.control & multiple_writeback) != 0)
        state.registers[rn] = transfer.updated_base;
    if (load && (registers & 0x8000U) != 0) {
        state.cpsr = (state.cpsr & ~0x20U) | ((target & 1U) << 5U);
        state.registers[15] = target & ((target & 1U) ? ~1U : ~3U);
    } else
        state.registers[15] = pc + ((transfer.control >> 24U) & 7U);
    return { };
}
}
