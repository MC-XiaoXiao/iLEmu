/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"

namespace ilemu::execution::arm64 {
struct FlagEffects {
    unsigned read = 0, possible_write = 0, guaranteed_write = 0;
    bool boundary = true;
};
// NZCV dependencies within uninterrupted register-only trace segments.
FlagEffects flag_effects(const arm::Instruction&) noexcept;
}
