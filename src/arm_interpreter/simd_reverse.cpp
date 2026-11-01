/* SPDX-License-Identifier: MPL-2.0 */
#include "simd.hpp"
#include <array>

namespace ilemu::execution {
void reverse_vector(CpuThreadState& state, const arm::VectorReverseOperands& v)
{
    const auto words = v.quad ? 4U : 2U;
    std::array<std::uint32_t, 4> input;
    for (unsigned i = 0; i < words; ++i)
        input[i] = state.extension_registers[v.source * 2U + i];
    const auto reverse_mask = (v.block_bits - v.element_bits) / 8U;
    for (unsigned i = 0; i < words; ++i) {
        std::uint32_t value = 0;
        for (unsigned byte = 0; byte < 4; ++byte) {
            const auto source = (i * 4U + byte) ^ reverse_mask;
            value |= ((input[source / 4U] >> ((source % 4U) * 8U)) & 255U)
                     << (byte * 8U);
        }
        state.extension_registers[v.destination * 2U + i] = value;
    }
}
}
