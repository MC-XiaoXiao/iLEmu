/* SPDX-License-Identifier: MPL-2.0 */
#include "floating_decode.hpp"
namespace ilemu::arm {
bool decode_floating_conversion(Instruction& out, std::uint32_t word,
    bool thumb, execution::VfpRegisterProfile bank,
    execution::ArmInstructionProfile architecture) noexcept
{
    if (thumb ? (word >> 28U) != 14U : out.condition == 15)
        return false;
    FloatingConversionOperands v;
    const bool size = (word & 256U) != 0;
    if ((word & 0x0fbf0e50U) == 0x0eb80a40U) {
        v.operation = FloatingConversionOperation::IntegerToFloat;
        v.destination_double = size;
        v.signed_integer = (word & 128U) != 0;
    } else if ((word & 0x0fbe0e50U) == 0x0ebc0a40U) {
        v.operation = FloatingConversionOperation::FloatToInteger;
        v.source_double = size;
        v.signed_integer = (word & 0x10000U) != 0;
        v.round_zero = (word & 128U) != 0;
    } else if ((word & 0x0fbf0ed0U) == 0x0eb70ac0U) {
        v.source_double = size;
        v.destination_double = !size;
    } else
        return false;
    const auto d = (word >> 12U) & 15U, m = word & 15U;
    const auto high_d = (word >> 22U) & 1U, high_m = (word >> 5U) & 1U;
    v.destination =
        v.destination_double ? (d + high_d * 16U) * 2U : d * 2U + high_d;
    v.source = v.source_double ? (m + high_m * 16U) * 2U : m * 2U + high_m;
    if ((thumb && architecture == execution::ArmInstructionProfile::Armv6) ||
        (bank == execution::VfpRegisterProfile::D16 &&
            ((v.source_double && v.source >= 32) ||
                (v.destination_double && v.destination >= 32))))
        return true;
    out.kind = InstructionKind::FloatingConversion;
    out.vector = v;
    return true;
}
}
