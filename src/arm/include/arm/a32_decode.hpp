/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
namespace ilemu::arm {
Instruction decode_a32(std::uint32_t word) noexcept;
}
