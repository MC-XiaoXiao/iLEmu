/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"

namespace ilemu::arm {
bool decode_simd_bitwise(Instruction&, std::uint32_t, bool thumb) noexcept;
}
