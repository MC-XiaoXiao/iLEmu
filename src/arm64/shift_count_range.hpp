/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "compiler.hpp"
#include <array>
#include <optional>
namespace ilemu::execution::arm64 {
// Valid only in a cycle that cannot write this count register. Native entry
// classifies its low byte; neither a guest value nor a host pointer is retained.
enum class ShiftCountRange { BelowWord, AtLeastWord, Zero, Small, Word, Large };
struct RegisterShiftRange {
    unsigned reg;
    ShiftCountRange range;
    unsigned carry_kinds = 0;
    bool precise() const
    {
        return range != ShiftCountRange::BelowWord &&
               range != ShiftCountRange::AtLeastWord;
    }
    bool at_least_word() const
    {
        return range == ShiftCountRange::AtLeastWord ||
               range == ShiftCountRange::Word || range == ShiftCountRange::Large;
    }
    bool accepts(const arm::Instruction& inst) const
    {
        return inst.register_shift && inst.rs == reg &&
            (precise() || inst.shift != arm::ShiftKind::Ror);
    }
};
class GuardedShiftTrace {
public:
    static std::optional<CompiledTrace> select(unsigned count_register,
        CompiledTrace below_word, CompiledTrace at_least_word);
    // Array order: zero, 1..31, 32, 33..255. Equal bodies share one copy.
    static std::optional<CompiledTrace> select(unsigned count_register,
        std::array<CompiledTrace, 4>);
};
}
