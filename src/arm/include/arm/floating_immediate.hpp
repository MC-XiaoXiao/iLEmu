/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
namespace ilemu::arm {
// VFPExpandImm: the finite normalized constants shared by A32/T32 and A64.
constexpr std::uint64_t expand_floating_immediate(
    const FloatingImmediateOperands& v) noexcept
{
    const std::uint64_t exponent = ((v.imm8 & 64U)
        ? (v.wide ? 0x3fcU : 0x7cU) : (v.wide ? 0x400U : 0x80U)) |
        ((v.imm8 >> 4U) & 3U);
    return (static_cast<std::uint64_t>(v.imm8 >> 7U) << (v.wide ? 63U : 31U)) |
        (exponent << (v.wide ? 52U : 23U)) |
        (std::uint64_t { v.imm8 & 15U } << (v.wide ? 48U : 19U));
}
}
