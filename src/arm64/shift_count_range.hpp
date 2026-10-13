/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "compiler.hpp"
#include <optional>
namespace ilemu::execution::arm64 {
// Valid only in a cycle that cannot write this count register. Native entry
// classifies its low byte; neither a guest value nor a host pointer is retained.
struct RegisterShiftRange {
    unsigned reg;
    bool at_least_word;
    bool accepts(const arm::Instruction& inst) const
    {
        return inst.register_shift && inst.rs == reg &&
            inst.shift != arm::ShiftKind::Ror;
    }
};
class GuardedShiftTrace {
public:
    static std::optional<CompiledTrace> select(unsigned count_register,
        CompiledTrace below_word, CompiledTrace at_least_word);
};
}
