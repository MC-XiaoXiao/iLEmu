/* SPDX-License-Identifier: MPL-2.0 */
#include "simd.hpp"
#include "arm/simd_immediate.hpp"
#include "arm/floating_arithmetic.hpp"
#include <array>

namespace ilemu::execution {
void floating_vector(
    CpuThreadState& state, const arm::VectorFloatingArithmeticOperands& v)
{
    const arm::FloatingArithmeticOperands scalar {
        0, 0, 0, v.operation, false
    };
    // Advanced SIMD uses standard controls, independently of FPSCR rounding,
    // vector length/stride and trap enables. Exceptions still accumulate in
    // the actual FPSCR; neighbouring D lanes and all control bits survive.
    constexpr std::uint32_t standard_controls = 0x03000000U;
    for (unsigned i = 0; i < (v.quad ? 4U : 2U); ++i) {
        const auto result = arm::calculate_floating(scalar,
            state.extension_registers[v.first * 2U + i],
            state.extension_registers[v.second * 2U + i], 0, standard_controls);
        state.extension_registers[v.destination * 2U + i] =
            static_cast<std::uint32_t>(result.value);
        state.fpscr |= result.exceptions;
    }
}
void move_core_vector(CpuThreadState& state, const arm::VectorCoreMoveOperands& v)
{
    if (v.pair) {
        if (v.to_core) {
            state.registers[v.core] = state.extension_registers[v.first_word];
            state.registers[v.core2] = state.extension_registers[v.first_word + 1U];
        } else {
            state.extension_registers[v.first_word] = state.registers[v.core];
            state.extension_registers[v.first_word + 1U] = state.registers[v.core2];
        }
        return;
    }
    const auto mask = 0xffffffffU >> (32U - v.element_bits);
    auto& word = state.extension_registers[v.first_word];
    if (v.to_core) {
        const auto value = (word >> v.bit_offset) & mask;
        const auto sign = v.sign_extend ? 1U << (v.element_bits - 1U) : 0U;
        state.registers[v.core] = (value ^ sign) - sign;
    } else
        word = (word & ~(mask << v.bit_offset)) |
               ((state.registers[v.core] & mask) << v.bit_offset);
}
void duplicate_vector(
    CpuThreadState& state, const arm::VectorDuplicateOperands& v)
{
    const auto bit = v.lane * v.element_bits;
    auto value = v.core_source
        ? state.registers[v.source]
        : state.extension_registers[v.source * 2U + bit / 32U] >> (bit % 32U);
    if (v.element_bits == 8)
        value = (value & 255U) * 0x01010101U;
    else if (v.element_bits == 16)
        value = (value & 65535U) * 0x00010001U;
    for (unsigned i = 0; i < (v.quad ? 4U : 2U); ++i)
        state.extension_registers[v.destination * 2U + i] = value;
}
void immediate_vector(CpuThreadState& state, const arm::VectorImmediateOperands& v)
{
    auto value = arm::expand_simd_immediate(v);
    if (v.op && v.cmode < 14)
        value = ~value;
    const bool modify = v.cmode < 12 && (v.cmode & 1U) != 0;
    for (unsigned i = 0; i < (v.quad ? 4U : 2U); ++i) {
        const auto word = static_cast<std::uint32_t>(value >> ((i % 2U) * 32U));
        auto& destination = state.extension_registers[v.destination * 2U + i];
        if (modify)
            destination = v.op ? destination & word : destination | word;
        else
            destination = word;
    }
}
void bitwise_vector(CpuThreadState& state, const arm::VectorBitwiseOperands& v)
{
    using Op = arm::VectorBitwiseOperation;
    for (unsigned i = 0; i < (v.quad ? 4U : 2U); ++i) {
        const auto a = state.extension_registers[v.first * 2U + i];
        const auto b = state.extension_registers[v.second * 2U + i];
        const auto d = state.extension_registers[v.destination * 2U + i];
        std::uint32_t result = 0;
        switch (v.operation) {
        case Op::And: result = a & b; break;
        case Op::BitClear: result = a & ~b; break;
        case Op::Or: result = a | b; break;
        case Op::OrNot: result = a | ~b; break;
        case Op::Xor: result = a ^ b; break;
        case Op::Select: result = (d & a) | (~d & b); break;
        case Op::InsertIfTrue: result = (b & a) | (~b & d); break;
        case Op::InsertIfFalse: result = (~b & a) | (b & d); break;
        case Op::Not: result = ~b; break;
        }
        state.extension_registers[v.destination * 2U + i] = result;
    }
}
void extract_vector(CpuThreadState& state, const arm::VectorExtractOperands& v)
{
    const auto words = v.quad ? 4U : 2U;
    // Snapshot both vectors before writing any aliased destination word.
    std::array<std::uint32_t, 8> input;
    for (unsigned i = 0; i < words; ++i) {
        input[i] = state.extension_registers[v.first * 2U + i];
        input[words + i] = state.extension_registers[v.second * 2U + i];
    }
    const auto shift = (v.byte_offset % 4U) * 8U;
    for (unsigned i = 0; i < words; ++i) {
        const auto index = v.byte_offset / 4U + i;
        auto value = input[index] >> shift;
        if (shift != 0)
            value |= input[index + 1U] << (32U - shift);
        state.extension_registers[v.destination * 2U + i] = value;
    }
}
}
