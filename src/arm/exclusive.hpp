/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
namespace ilemu::arm {
bool decode_exclusive(Instruction&, std::uint32_t word, bool thumb) noexcept;
}
