/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/integer_transform.hpp"
#include <bit>

namespace ilemu::arm {
namespace {
    std::uint32_t extend(std::uint32_t value, unsigned bits, bool sign)
    {
        const auto mask = (1U << bits) - 1U;
        const auto top = sign ? 1U << (bits - 1U) : 0U;
        return ((value & mask) ^ top) - top;
    }
    std::uint32_t reverse_halfwords(std::uint32_t value)
    { return ((value & 0x00ff00ffU) << 8U) | ((value >> 8U) & 0x00ff00ffU); }
}
std::uint32_t integer_transform(const Instruction& inst, std::uint32_t source,
    std::uint32_t addend) noexcept
{
    using Operation = IntegerTransformOperation;
    const auto value = std::rotr(source, static_cast<int>(inst.shift_amount));
    switch (static_cast<Operation>(inst.opcode)) {
    case Operation::ExtendByte:
    case Operation::ExtendHalfword:
        return extend(value, inst.opcode == static_cast<unsigned>(Operation::ExtendByte)
                ? 8U : 16U, inst.sign_extend) + (inst.accumulate ? addend : 0U);
    case Operation::ExtendBytePair: {
        const auto low = extend(value, 8, inst.sign_extend);
        const auto high = extend(value >> 16U, 8, inst.sign_extend);
        // Add the halfwords independently: a low-lane carry never reaches
        // the high lane. These instructions do not modify NZCV/Q/GE.
        return ((low + (inst.accumulate ? addend & 0xffffU : 0U)) & 0xffffU) |
            ((high + (inst.accumulate ? addend >> 16U : 0U)) << 16U);
    }
    case Operation::ReverseBytes:
        return std::rotl(reverse_halfwords(value), 16);
    case Operation::ReverseHalfwords:
        return reverse_halfwords(value);
    case Operation::ReverseSignedHalfword:
        return extend(reverse_halfwords(value), 16, true);
    case Operation::ReverseBits: {
        auto bits = ((value & 0x55555555U) << 1U) | ((value >> 1U) & 0x55555555U);
        bits = ((bits & 0x33333333U) << 2U) | ((bits >> 2U) & 0x33333333U);
        bits = ((bits & 0x0f0f0f0fU) << 4U) | ((bits >> 4U) & 0x0f0f0f0fU);
        return std::rotl(reverse_halfwords(bits), 16);
    }
    }
    return 0;
}
}
