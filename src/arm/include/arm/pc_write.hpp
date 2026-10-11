/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/instruction_profile.hpp"
#include <optional>

namespace ilemu::arm {
constexpr bool alu_writes_pc(const Instruction& inst) noexcept
{
    return inst.kind == InstructionKind::DataProcessing && inst.rd == 15 &&
           (inst.opcode < 8 || inst.opcode > 11);
}
constexpr bool alu_interworks(
    bool thumb, execution::AluPcWriteProfile profile) noexcept
{
    return !thumb && profile == execution::AluPcWriteProfile::Interworking;
}
struct PcWrite {
    std::uint32_t address;
    bool thumb;
};
constexpr std::optional<PcWrite> alu_write_pc(std::uint32_t value, bool thumb,
    execution::AluPcWriteProfile profile) noexcept
{
    if (alu_interworks(thumb, profile)) {
        if ((value & 3U) == 2U)
            return { };
        thumb = (value & 1U) != 0;
    }
    return PcWrite { value & (thumb ? ~1U : ~3U), thumb };
}
}
