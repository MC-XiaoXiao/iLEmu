/* SPDX-License-Identifier: MPL-2.0 */
#include "simd.hpp"

namespace ilemu::execution {
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
