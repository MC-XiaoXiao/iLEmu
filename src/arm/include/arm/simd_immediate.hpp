/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
namespace ilemu::arm {
// The expanded operand before VMVN/VBIC inversion, in register bit order.
std::uint64_t expand_simd_immediate(const VectorImmediateOperands&) noexcept;
}
