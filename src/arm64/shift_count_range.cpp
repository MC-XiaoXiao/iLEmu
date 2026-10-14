/* SPDX-License-Identifier: MPL-2.0 */
#include "shift_count_range.hpp"
#include <oaknut/oaknut.hpp>
namespace ilemu::execution::arm64 {
namespace {
bool matching_metadata(const CompiledTrace& first, const CompiledTrace& second)
{
    return !first.stop && !second.stop &&
        first.maximum_ticks == second.maximum_ticks &&
        first.instructions == second.instructions && first.closed == second.closed &&
        first.accesses_memory == second.accesses_memory &&
        first.ends_with_stop == second.ends_with_stop &&
        first.first_instruction == second.first_instruction;
}
}
std::optional<CompiledTrace> GuardedShiftTrace::select(unsigned reg,
    CompiledTrace below, CompiledTrace above)
{
    if (!matching_metadata(below, above))
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
std::optional<CompiledTrace> GuardedShiftTrace::select(
    unsigned reg, std::array<CompiledTrace, 4> traces)
{
    std::array<unsigned, 4> canonical {};
    std::size_t words = 32; // Entry selection and per-body alignment.
    unsigned unique = 0;
    for (unsigned i = 0; i < traces.size(); ++i) {
        if (!matching_metadata(traces[0], traces[i]))
            return {};
        canonical[i] = i;
        for (unsigned j = 0; j < i; ++j) {
            if (traces[i].words == traces[j].words) {
                canonical[i] = canonical[j];
                break;
            }
        }
        if (canonical[i] == i) {
            ++unique;
            words += traces[i].words.size();
        }
    }
    // Leave room for cold entries and allocation/metadata rounding within
    // the existing minimum cache budget. This optimization cannot raise it.
    if (words > 128U * 1024U / sizeof(std::uint32_t))
        return {};
    if (unique == 1)
        return std::move(traces[0]);
    using namespace oaknut;
    using namespace oaknut::util;
    CompiledTrace result;
    static_cast<TraceMetadata&>(result) = traces[0];
    result.words.reserve(words);
    VectorCodeGenerator code(result.words);
    std::array<Label, 4> labels;
    code.LDR(W16, X0, reg * sizeof(std::uint32_t));
    code.AND(W16, W16, 255);
    code.CBZ(W16, labels[canonical[0]]);
    code.CMP(W16, 32);
    code.B(Cond::CC, labels[canonical[1]]);
    code.B(Cond::EQ, labels[canonical[2]]);
    code.B(labels[canonical[3]]);
    for (unsigned i = 0; i < traces.size(); ++i) {
        if (canonical[i] != i)
            continue;
        code.align(16);
        code.l(labels[i]);
        result.words.insert(result.words.end(), traces[i].words.begin(), traces[i].words.end());
    }
    return result;
}
}
