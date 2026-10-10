/* SPDX-License-Identifier: MPL-2.0 */
#include "simd.hpp"

namespace ilemu::execution {
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
}
