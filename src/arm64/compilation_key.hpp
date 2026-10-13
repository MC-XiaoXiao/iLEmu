/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/run.hpp"
#include "cycle_tier.hpp"
#include <tuple>
namespace ilemu::execution::arm64 {
using TranslationKey = std::tuple<std::uint32_t, bool, bool, bool, unsigned, bool,
    std::uint64_t, ThumbArithmeticProfile, VfpRegisterProfile,
    ArmInstructionProfile, FloatingExceptionProfile, AluPcWriteProfile,
    VfpInstructionProfile, IntegerDivideProfile, ThumbMemoryProfile,
    AdvancedSimdProfile>;
struct CompilationKey {
    CompilationIdentity identity;
    TranslationKey entry;
    CycleExpansion expansion;
    std::uint64_t region_begin, region_end;
    auto operator<=>(const CompilationKey&) const = default;
};
}
