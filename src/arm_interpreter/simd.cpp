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
}
