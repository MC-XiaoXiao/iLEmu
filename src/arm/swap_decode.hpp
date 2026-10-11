/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
namespace ilemu::arm {
bool decode_swap(Instruction&, std::uint32_t) noexcept;
}
