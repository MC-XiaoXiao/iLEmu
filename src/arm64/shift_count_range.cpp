/* SPDX-License-Identifier: MPL-2.0 */
#include "shift_count_range.hpp"
#include <oaknut/oaknut.hpp>
namespace ilemu::execution::arm64 {
std::optional<CompiledTrace> GuardedShiftTrace::select(unsigned reg,
    CompiledTrace below, CompiledTrace above)
{
    if (below.stop || above.stop ||
        below.maximum_ticks != above.maximum_ticks ||
        below.instructions != above.instructions ||
        below.closed != above.closed ||
        below.accesses_memory != above.accesses_memory ||
        below.ends_with_stop != above.ends_with_stop ||
        below.first_instruction != above.first_instruction)
        return {};
    if (below.words == above.words)
        return below;
    using namespace oaknut;
    using namespace oaknut::util;
    CompiledTrace result;
    static_cast<TraceMetadata&>(result) = below;
    VectorCodeGenerator code(result.words);
    Label small;
    // A32/T32 register shifts use the low eight bits. Bits 5..7 classify
    // zero/ordinary counts versus counts at least one 32-bit guest word.
    // Arguments and guest flags remain intact until either native prologue.
    code.LDR(W16, X0, reg * sizeof(std::uint32_t));
    code.AND(W16, W16, 0xe0U);
    code.CBZ(W16, small);
    code.align(16);
    result.words.insert(result.words.end(), above.words.begin(), above.words.end());
    code.align(16);
    code.l(small);
    result.words.insert(result.words.end(), below.words.begin(), below.words.end());
    return result;
}
}
