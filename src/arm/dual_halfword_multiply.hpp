/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/multiply.hpp"
namespace ilemu::arm {
MultiplyResult dual_halfword_multiply(const Instruction&, std::uint32_t first,
    std::uint32_t second, std::uint32_t low, std::uint32_t high_or_addend,
    std::uint32_t cpsr) noexcept;
}
