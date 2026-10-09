/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include <optional>
namespace ilemu::arm {
constexpr bool thumb_is_wide(std::uint16_t first) noexcept
{
    return (first & 0xf800U) >= 0xe800U;
}
constexpr unsigned it_state(std::uint32_t cpsr) noexcept
{
    return ((cpsr >> 8U) & 0xfcU) | ((cpsr >> 25U) & 3U);
}
constexpr unsigned advance_it(unsigned value) noexcept
{
    return (value & 7U) == 0 ? 0U : (value & 0xe0U) | ((value << 1U) & 0x1fU);
}
constexpr std::uint32_t with_it_state(
    std::uint32_t cpsr, unsigned value) noexcept
{
    return (cpsr & ~0x0600fc00U) | ((value & 0xfcU) << 8U) |
           ((value & 3U) << 25U);
}
Instruction decode_t32(std::uint16_t first, std::optional<std::uint16_t> second,
    unsigned it) noexcept;
}
