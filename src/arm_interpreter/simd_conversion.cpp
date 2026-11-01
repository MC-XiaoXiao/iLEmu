/* SPDX-License-Identifier: MPL-2.0 */
#include "simd.hpp"
#include "arm/floating_conversion.hpp"
#include <array>

namespace ilemu::execution {
void convert_vector(CpuThreadState& state, const arm::VectorFloatingConversionOperands& v)
{
    arm::FloatingConversionOperands scalar;
    scalar.operation = v.to_integer ? arm::FloatingConversionOperation::FloatToInteger
                                    : arm::FloatingConversionOperation::IntegerToFloat;
    scalar.signed_integer = !v.unsigned_value;
    scalar.round_zero = v.to_integer;
    const auto lanes = v.quad ? 4U : 2U;
    std::array<std::uint32_t, 4> input;
    for (unsigned i = 0; i < lanes; ++i)
        input[i] = state.extension_registers[v.source * 2U + i];
    for (unsigned i = 0; i < lanes; ++i) {
        const auto result = arm::convert_floating(scalar, input[i], 0x03000000U);
        state.extension_registers[v.destination * 2U + i] =
            static_cast<std::uint32_t>(result.value);
        state.fpscr |= result.exceptions;
    }
}
}
