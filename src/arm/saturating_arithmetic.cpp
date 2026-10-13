/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/saturating_arithmetic.hpp"
#include "saturating_arithmetic_decode.hpp"
#include <algorithm>
#include <bit>

namespace ilemu::arm {
bool decode_saturating_arithmetic(Instruction& out, std::uint32_t word,
    bool thumb, execution::ArmInstructionProfile architecture) noexcept
{
    using Op = SaturatingOperation;
    Op operation;
    unsigned first, second = 0, destination;
    bool unsigned_result = false;
    if (thumb) {
        first = (word >> 16U) & 15U;
        destination = (word >> 8U) & 15U;
        unsigned_result = (word & (1U << 23U)) != 0;
        if ((word & 0xfff0f0c0U) == 0xfa80f080U) {
            constexpr Op operations[] = { Op::Add, Op::DoubleAdd, Op::Subtract,
                Op::DoubleSubtract };
            operation = operations[(word >> 4U) & 3U];
            second = first;
            first = word & 15U;
        } else if ((word & 0xff70f0f0U) == 0xf3200000U) {
            operation =
                unsigned_result ? Op::UnsignedHalfwords : Op::SignedHalfwords;
            out.immediate = (word & 15U) + unsigned(!unsigned_result);
        } else if ((word & 0xff508020U) == 0xf3000000U) {
            operation = unsigned_result ? Op::Unsigned : Op::Signed;
            out.immediate = (word & 31U) + unsigned(!unsigned_result);
            out.shift =
                (word & (1U << 21U)) != 0 ? ShiftKind::Asr : ShiftKind::Lsl;
            out.shift_amount = ((word >> 10U) & 28U) | ((word >> 6U) & 3U);
            // ASR with a zero encoding is the parallel halfword family.
            if (out.shift == ShiftKind::Asr && out.shift_amount == 0)
                return true;
        } else {
            return false;
        }
        if (!execution::has_armv6t2_extensions(architecture) || first == 13 ||
            second == 13 || destination == 13)
            return true;
    } else {
        if (out.condition == 15)
            return false;
        first = word & 15U;
        destination = (word >> 12U) & 15U;
        unsigned_result = (word & (1U << 22U)) != 0;
        if ((word & 0x0f900ff0U) == 0x01000050U) {
            constexpr Op operations[] = { Op::Add, Op::Subtract, Op::DoubleAdd,
                Op::DoubleSubtract };
            operation = operations[(word >> 21U) & 3U];
            second = (word >> 16U) & 15U;
        } else if ((word & 0x0fb00ff0U) == 0x06a00f30U) {
            operation =
                unsigned_result ? Op::UnsignedHalfwords : Op::SignedHalfwords;
            out.immediate = ((word >> 16U) & 15U) + unsigned(!unsigned_result);
        } else if ((word & 0x0fa00030U) == 0x06a00010U) {
            operation = unsigned_result ? Op::Unsigned : Op::Signed;
            out.immediate = ((word >> 16U) & 31U) + unsigned(!unsigned_result);
            out.shift =
                (word & (1U << 6U)) != 0 ? ShiftKind::Asr : ShiftKind::Lsl;
            out.shift_amount = (word >> 7U) & 31U;
        } else {
            return false;
        }
    }
    if (first == 15 || second == 15 || destination == 15)
        return true;
    out.kind = InstructionKind::SaturatingArithmetic;
    out.opcode = static_cast<unsigned>(operation);
    out.rn = first;
    out.rm = second;
    out.rd = destination;
    return true;
}

AluResult saturating_arithmetic(const Instruction& inst, std::uint32_t first,
    std::uint32_t second, std::uint32_t cpsr) noexcept
{
    using Op = SaturatingOperation;
    const auto operation = static_cast<Op>(inst.opcode);
    const auto saturate = [&](std::int64_t value, unsigned bits,
                              bool is_signed) {
        const auto minimum =
            is_signed ? -(std::int64_t { 1 } << (bits - 1U)) : 0;
        const auto maximum =
            (std::int64_t { 1 } << (bits - unsigned(is_signed))) - 1;
        const auto clamped = std::clamp(value, minimum, maximum);
        if (clamped != value)
            cpsr |= 1U << 27U;
        return clamped;
    };
    if (operation == Op::Signed || operation == Op::Unsigned) {
        const auto operand =
            shift(first, inst.shift, inst.shift_amount, false, false).value;
        return { static_cast<std::uint32_t>(
                     saturate(std::bit_cast<std::int32_t>(operand),
                         inst.immediate, operation == Op::Signed)),
            cpsr };
    }
    if (operation == Op::SignedHalfwords ||
        operation == Op::UnsignedHalfwords) {
        std::uint32_t result = 0;
        for (unsigned offset : { 0U, 16U }) {
            const auto value = std::bit_cast<std::int16_t>(
                static_cast<std::uint16_t>(first >> offset));
            const auto lane = saturate(
                value, inst.immediate, operation == Op::SignedHalfwords);
            result |= (static_cast<std::uint32_t>(lane) & 0xffffU) << offset;
        }
        return { result, cpsr };
    }
    auto addend = std::int64_t { std::bit_cast<std::int32_t>(second) };
    if (operation == Op::DoubleAdd || operation == Op::DoubleSubtract)
        addend = saturate(2 * addend, 32, true);
    const bool subtract =
        operation == Op::Subtract || operation == Op::DoubleSubtract;
    const auto value = std::int64_t { std::bit_cast<std::int32_t>(first) } +
                       (subtract ? -addend : addend);
    return { static_cast<std::uint32_t>(saturate(value, 32, true)), cpsr };
}
}
