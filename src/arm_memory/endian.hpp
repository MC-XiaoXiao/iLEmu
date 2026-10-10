/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <cstdint>
namespace ilemu::execution::arm_memory {
inline std::uint32_t reverse(std::uint32_t value, unsigned size)
{
    if (size == 1)
        return value;
    if (size == 2)
        return ((value & 255U) << 8U) | ((value >> 8U) & 255U);
    return ((value & 255U) << 24U) | ((value & 0xff00U) << 8U) |
           ((value >> 8U) & 0xff00U) | (value >> 24U);
}
}
