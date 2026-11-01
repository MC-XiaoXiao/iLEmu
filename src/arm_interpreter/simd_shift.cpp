/* SPDX-License-Identifier: MPL-2.0 */
#include "simd.hpp"
#include <array>

namespace ilemu::execution {
void shift_vector(CpuThreadState& state, const arm::VectorShiftImmediateOperands& v)
{
    const auto mask = UINT64_MAX >> (64U - v.element_bits);
    const auto read = [&](unsigned d, unsigned bit) {
        const auto word = d * 2U + bit / 32U;
        auto value = std::uint64_t { state.extension_registers[word] } >> (bit % 32U);
        if (v.element_bits == 64)
            value |= std::uint64_t { state.extension_registers[word + 1U] } << 32U;
        return value & mask;
    };
    std::array<std::uint32_t, 4> result {};
    for (unsigned bit = 0; bit < (v.quad ? 128U : 64U); bit += v.element_bits) {
        const auto input = read(v.source, bit);
        const auto destination = read(v.destination, bit);
        std::uint64_t value;
        if (v.right) {
            const bool negative = !v.unsigned_value && !v.insert &&
                (input & (std::uint64_t { 1 } << (v.element_bits - 1U))) != 0;
            const auto extended = negative ? input | ~mask : input;
            value = v.amount == 64 ? (negative ? UINT64_MAX : 0U)
                : negative ? static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(extended) >> v.amount)
                : input >> v.amount;
            if (v.rounding)
                value += (input >> (v.amount - 1U)) & 1U;
            if (v.insert)
                value |= destination & (v.amount == 64 ? mask : mask & ~(mask >> v.amount));
        } else {
            value = input << v.amount;
            if (v.insert)
                value |= destination & ((std::uint64_t { 1 } << v.amount) - 1U);
        }
        if (v.accumulate)
            value += destination;
        value &= mask;
        result[bit / 32U] |= static_cast<std::uint32_t>(value) << (bit % 32U);
        if (v.element_bits == 64)
            result[bit / 32U + 1U] = static_cast<std::uint32_t>(value >> 32U);
    }
    for (unsigned i = 0; i < (v.quad ? 4U : 2U); ++i)
        state.extension_registers[v.destination * 2U + i] = result[i];
}
}
