/* SPDX-License-Identifier: MPL-2.0 */
#include "simd.hpp"
#include <array>
namespace ilemu::execution {
void integer_vector(CpuThreadState& state, const arm::VectorIntegerOperands& v)
{
    const auto& words = state.extension_registers;
    const auto mask = UINT64_MAX >> (64U - v.element_bits);
    const auto read = [&](unsigned d, unsigned bit) {
        const auto word = d * 2U + bit / 32U;
        auto value = std::uint64_t { words[word] } >> (bit % 32U);
        if (v.element_bits == 64)
            value |= std::uint64_t { words[word + 1U] } << 32U;
        return value & mask;
    };
    // Publish only after all source/accumulator lanes have been read, including
    // D operands that alias different halves of the same Q register.
    std::array<std::uint32_t, 4> result {};
    for (unsigned bit = 0; bit < (v.quad ? 128U : 64U); bit += v.element_bits) {
        const auto a = read(v.first, bit), b = read(v.second, bit);
        std::uint64_t value = 0;
        using Op = arm::VectorIntegerOperation;
        switch (v.operation) {
        case Op::Add: value = a + b; break;
        case Op::Subtract: value = a - b; break;
        case Op::Multiply: value = a * b; break;
        case Op::MultiplyAdd: value = read(v.destination, bit) + a * b; break;
        case Op::MultiplySubtract: value = read(v.destination, bit) - a * b; break;
        }
        value &= mask;
        result[bit / 32U] |= static_cast<std::uint32_t>(value) << (bit % 32U);
        if (v.element_bits == 64)
            result[bit / 32U + 1U] = static_cast<std::uint32_t>(value >> 32U);
    }
    for (unsigned i = 0; i < (v.quad ? 4U : 2U); ++i)
        state.extension_registers[v.destination * 2U + i] = result[i];
}
}
