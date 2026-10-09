/* SPDX-License-Identifier: MPL-2.0 */
#include "arm_memory/transfer.hpp"
#include "arm/integer_semantics.hpp"

namespace ilemu::execution::arm_memory {
namespace {
    std::uint32_t reverse(std::uint32_t value, unsigned size)
    {
        if (size == 1)
            return value;
        if (size == 2)
            return ((value & 255U) << 8U) | ((value >> 8U) & 255U);
        return ((value & 255U) << 24U) | ((value & 0xff00U) << 8U) |
               ((value >> 8U) & 0xff00U) | (value >> 24U);
    }
}
Transfer prepare(const CpuThreadState& state, const arm::Instruction& inst,
    std::uint32_t pc_store_offset)
{
    const auto read = [&](unsigned r) {
        if (r != 15)
            return state.registers[r];
        const auto pc = state.registers[15] + inst.pc_offset;
        return inst.align_pc ? pc & ~3U : pc;
    };
    const auto offset =
        inst.immediate_operand
            ? inst.immediate
            : arm::shift(read(inst.rm), inst.shift, inst.shift_amount,
                  (state.cpsr & (1U << 29)) != 0, false)
                  .value;
    const auto base = read(inst.rn);
    const auto updated = inst.add ? base + offset : base - offset;
    return { 0, inst.index ? updated : base, updated,
        inst.rd == 15 ? state.registers[15] + pc_store_offset : read(inst.rd),
        control(inst) };
}
Completion complete(
    CpuThreadState& state, MemoryAccess& memory, const Transfer& transfer)
{
    const auto size = transfer.control & 7U;
    const auto rd = (transfer.control >> 8U) & 15U;
    const auto rn = (transfer.control >> 12U) & 15U;
    const bool load = (transfer.control & load_bit) != 0;
    const bool big_endian = (state.cpsr & (1U << 9)) != 0;
    if (load && rd == 15 && (transfer.address & 3U) != 0)
        return { StopReason::UnsupportedInstruction, { } };
    std::optional<MemoryFault> fault;
    auto value = transfer.value;
    if (load) {
        const auto result =
            memory.read(transfer.address, static_cast<AccessSize>(size));
        fault = result.fault;
        value = big_endian ? reverse(result.value, size) : result.value;
        if (size != 4)
            value &= size == 1 ? 0xffU : 0xffffU;
        if (!fault && (transfer.control & signed_bit) != 0) {
            const auto sign = size == 1 ? 0x80U : 0x8000U;
            const auto mask = size == 1 ? 0xffU : 0xffffU;
            value = ((value & mask) ^ sign) - sign;
        }
    } else {
        fault = memory.write(transfer.address, static_cast<AccessSize>(size),
            big_endian ? reverse(value, size) : value);
    }
    if (fault) {
        state.abort_state = { fault->status, fault->address };
        return { StopReason::DataFault, fault };
    }
    if (load && rd == 15 && (value & 3U) == 2U)
        return { StopReason::UnsupportedInstruction, { } };
    if ((transfer.control & writeback_bit) != 0)
        state.registers[rn] = transfer.updated_base;
    if (load && rd != 15)
        state.registers[rd] = value;
    if (load && rd == 15) {
        state.cpsr = (state.cpsr & ~(1U << 5)) | ((value & 1U) << 5);
        state.registers[15] = value & ((value & 1U) != 0 ? ~1U : ~3U);
    } else
        state.registers[15] += (transfer.control >> 16U) & 7U;
    return { };
}
}
