/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
namespace ilemu::arm {
// MRS masks execution state (J, T and IT). User control fields may expose
// their current values; the architecture otherwise leaves those UNKNOWN.
constexpr std::uint32_t read_program_status(std::uint32_t cpsr) noexcept
{
    return cpsr & 0xf8ff03dfU;
}
// User MSR cannot change privilege, interrupt masks or execution state.
// E remains user-writable even though changing it through MSR is deprecated.
constexpr std::uint32_t program_status_write_mask(unsigned bytes) noexcept
{
    return ((bytes & 8U) != 0 ? 0xf8000000U : 0U) |
           ((bytes & 4U) != 0 ? 0x000f0000U : 0U) |
           ((bytes & 2U) != 0 ? 0x00000200U : 0U);
}
constexpr bool program_status_writes_endianness(const Instruction& inst) noexcept
{
    return inst.kind == InstructionKind::SystemControl &&
           inst.system == SystemOperation::ProgramStatusRegister &&
           !inst.load && (inst.opcode & 2U) != 0;
}
}
