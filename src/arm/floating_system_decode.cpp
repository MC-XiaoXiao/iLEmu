/* SPDX-License-Identifier: MPL-2.0 */
#include "floating_system_decode.hpp"
namespace ilemu::arm {
bool decode_floating_system(Instruction& out, std::uint32_t word,
    bool thumb, execution::ArmInstructionProfile architecture) noexcept
{
    if ((word & 0x0fef0fffU) != 0x0ee10a10U ||
        (thumb ? (word >> 28U) != 14U : out.condition == 15))
        return false;
    const FloatingSystemOperands v {
        (word >> 12U) & 15U, (word & 0x100000U) != 0 };
    if ((thumb && (!execution::has_armv6t2_extensions(architecture) ||
                      v.core == 13)) || (!v.to_core && v.core == 15))
        return true;
    out.kind = InstructionKind::FloatingSystem;
    out.vector = v;
    return true;
}
}
