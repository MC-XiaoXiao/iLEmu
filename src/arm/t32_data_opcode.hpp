/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/instruction_profile.hpp"
#include <optional>

namespace ilemu::arm {
constexpr bool permits_general_sp_arithmetic(
    execution::ThumbArithmeticProfile profile, unsigned op) noexcept
{
    return profile ==
               execution::ThumbArithmeticProfile::GeneralPurposeStackPointer &&
           (op == 8 || op == 10 || op == 11 || op == 13 || op == 14);
}
// MOV/MVN aliases include the immediate-shift family. The core policy
// treats SP like a data register without relaxing PC or opcode allocation.
constexpr bool permits_general_sp_move(
    execution::ThumbArithmeticProfile profile, bool move) noexcept
{
    return move && profile ==
                       execution::ThumbArithmeticProfile::GeneralPurposeStackPointer;
}
// Shared logical/arithmetic operation and alias allocation for wide Thumb.
constexpr std::optional<unsigned> t32_data_opcode(
    unsigned op, bool move, bool test) noexcept
{
    switch (op) {
    case 0: return test ? 8U : 0U;
    case 1: return 14;
    case 2: return move ? 13U : 12U;
    case 3: return move ? 15U : opcode_orn;
    case 4: return test ? 9U : 1U;
    case 8: return test ? 11U : 4U;
    case 10: return 5;
    case 11: return 6;
    case 13: return test ? 10U : 2U;
    case 14: return 3;
    default: return { };
    }
}
}
