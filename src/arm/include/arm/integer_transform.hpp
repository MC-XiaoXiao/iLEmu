/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"

namespace ilemu::arm {
// IntegerTransform uses opcode for this operation, sign_extend for extension
// signedness, accumulate for the optional addend, and shift_amount for ROR.
// Bitfields instead use shift_amount for lsb and immediate for width. Insert
// uses the addend as the old destination and accumulate selects a source.
enum class IntegerTransformOperation {
    ExtendByte, ExtendHalfword, ExtendBytePair,
    ReverseBytes, ReverseHalfwords, ReverseSignedHalfword, ReverseBits,
    CountLeadingZeros, BitfieldInsert, BitfieldExtract
};
std::uint32_t integer_transform(const Instruction&, std::uint32_t source,
    std::uint32_t addend) noexcept;
}
