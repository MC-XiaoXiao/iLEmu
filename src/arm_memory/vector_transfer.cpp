/* SPDX-License-Identifier: MPL-2.0 */
#include "arm_memory/vector_transfer.hpp"
#include <algorithm>

namespace ilemu::execution::arm_memory {
VectorTransfer prepare_vector(const CpuThreadState& state,
    const arm::Instruction& inst, bool permits_unaligned)
{
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    const auto base = state.registers[inst.rn];
    const auto length = v.mode == arm::VectorTransferMode::Multiple
                            ? v.count * 8U
                            : v.element_size;
    const auto offset =
        inst.rm == 13 || inst.rm == 15 ? length : state.registers[inst.rm];
    return { 0, base, base + offset, vector_control(inst),
        std::max(v.alignment, permits_unaligned ? 1U : v.element_size) };
}
Completion complete_vector(
    CpuThreadState& state, MemoryAccess& memory, const VectorTransfer& transfer)
{
    const auto first = transfer.control & 31U;
    const auto count = (transfer.control >> 5U) & 7U;
    const auto size = (transfer.control >> 8U) & 15U;
    const auto rn = (transfer.control >> 12U) & 15U;
    const bool load = (transfer.control & vector_load) != 0;
    const bool big = (state.cpsr & 512U) != 0;
    const auto mode =
        static_cast<arm::VectorTransferMode>((transfer.control >> 23U) & 3U);
    const auto selected_lane = (transfer.control >> 25U) & 7U;
    const auto abort = [&](MemoryFault fault) {
        state.abort_state = { fault.status, fault.address };
        return Completion { StopReason::DataFault, fault };
    };
    if ((transfer.address & (transfer.alignment - 1U)) != 0)
        return abort({ transfer.address, 1U | (load ? 0U : 1U << 11U) });
    auto address = transfer.address;
    for (unsigned r = 0; r < count; ++r) {
        const auto start =
            mode == arm::VectorTransferMode::Lane ? selected_lane * size : 0U;
        const auto end =
            mode == arm::VectorTransferMode::Multiple ? 8U : start + size;
        for (unsigned lane = start; lane < end; lane += size) {
            // Build one element before committing a load. A 64-bit element
            // consists of two MemU word accesses; completed earlier elements
            // and stores remain visible if a later access faults.
            std::uint64_t value = 0;
            const auto index = (first + r) * 2U + lane / 4U;
            const auto shift = (lane % 4U) * 8U;
            if (!load) {
                value = state.extension_registers[index] >> shift;
                if (size == 8)
                    value |=
                        std::uint64_t { state.extension_registers[index + 1] }
                        << 32U;
            }
            const auto part_size = std::min(size, 4U);
            for (unsigned part = 0; part < size; part += part_size) {
                const auto bit = (big ? size - part - part_size : part) * 8U;
                if (load) {
                    const auto result = memory.read(
                        address + part, static_cast<AccessSize>(part_size));
                    if (result.fault)
                        return abort(*result.fault);
                    auto data = result.value;
                    if (big && part_size == 2)
                        data = ((data & 255U) << 8U) | ((data >> 8U) & 255U);
                    else if (big && part_size == 4)
                        data = ((data & 255U) << 24U) |
                               ((data & 0xff00U) << 8U) |
                               ((data >> 8U) & 0xff00U) | (data >> 24U);
                    const auto mask = part_size == 4
                                          ? UINT32_MAX
                                          : (1U << (part_size * 8U)) - 1U;
                    value |= std::uint64_t { data & mask } << bit;
                } else {
                    auto data = static_cast<std::uint32_t>(value >> bit);
                    if (big && part_size == 2)
                        data = ((data & 255U) << 8U) | ((data >> 8U) & 255U);
                    else if (big && part_size == 4)
                        data = ((data & 255U) << 24U) |
                               ((data & 0xff00U) << 8U) |
                               ((data >> 8U) & 0xff00U) | (data >> 24U);
                    if (const auto fault = memory.write(address + part,
                            static_cast<AccessSize>(part_size), data))
                        return abort(*fault);
                }
            }
            if (load) {
                if (mode == arm::VectorTransferMode::Replicate) {
                    const auto mask = (std::uint64_t { 1 } << (size * 8U)) - 1U;
                    std::uint64_t replicated = 0;
                    for (unsigned bit = 0; bit < 64; bit += size * 8U)
                        replicated |= (value & mask) << bit;
                    for (unsigned d = first; d < first + count; ++d) {
                        state.extension_registers[d * 2U] =
                            static_cast<std::uint32_t>(replicated);
                        state.extension_registers[d * 2U + 1U] =
                            static_cast<std::uint32_t>(replicated >> 32U);
                    }
                    break;
                }
                const auto mask =
                    size >= 4 ? UINT32_MAX : (1U << (size * 8U)) - 1U;
                auto& word = state.extension_registers[index];
                word = (word & ~(mask << shift)) |
                       ((static_cast<std::uint32_t>(value) & mask) << shift);
                if (size == 8)
                    state.extension_registers[index + 1] =
                        static_cast<std::uint32_t>(value >> 32U);
            }
            address += size;
        }
        if (mode == arm::VectorTransferMode::Replicate)
            break;
    }
    if ((transfer.control & vector_writeback) != 0)
        state.registers[rn] = transfer.updated_base;
    state.registers[15] += (transfer.control >> 20U) & 7U;
    return { };
}
}
