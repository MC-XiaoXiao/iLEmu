/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"

namespace ilemu::arm {
// Returns true for this encoding family, including its invalid allocations.
bool decode_simd_duplicate(Instruction&, std::uint32_t, bool thumb) noexcept;
}
