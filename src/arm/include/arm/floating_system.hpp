/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/instruction_profile.hpp"
#include <cstdint>
namespace ilemu::arm {
constexpr std::uint32_t fpscr_write_mask(
    execution::FloatingExceptionProfile profile) noexcept
{
    // Reserved bits 19, 14:13 and 6:5 read as zero after a write.
    return profile == execution::FloatingExceptionProfile::TrapEnables
               ? 0xfff79f9fU : 0xfff7009fU;
}
}
