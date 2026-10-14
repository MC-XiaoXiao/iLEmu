/* SPDX-License-Identifier: MPL-2.0 */
#include "scalar_register_plan.hpp"
#include <algorithm>
#include <bit>
namespace ilemu::execution::arm64 {
std::optional<unsigned> ScalarRegisterPlan::find(unsigned word, bool wide) const
{
    for (unsigned i = 0; i < count; ++i)
        if (entries[i].word == word && entries[i].wide == wide)
            return i;
    return { };
}
void ScalarRegisterPlan::Builder::use(
    unsigned word, bool wide, bool write, unsigned bonus, bool whole)
{
    auto& value = wide ? doubles_[word / 2U] : singles_[word];
    value.present = true;
    value.written |= write;
    value.whole |= whole;
    // Writes remove packed-bank dependencies; upper reads remove extraction.
    value.weight += bonus + (write ? 8U : (word % 4U != 0 ? 2U : 0U));
}
void ScalarRegisterPlan::Builder::observe(
    const arm::FloatingArithmeticOperands& v)
{
    using Op = arm::FloatingArithmeticOperation;
    use(v.destination, v.wide, true, 0, v.wide);
    use(v.first, v.wide, false, 0, v.wide);
    if (v.operation != Op::Move && v.operation != Op::Absolute &&
        v.operation != Op::Negate && v.operation != Op::SquareRoot)
        use(v.second, v.wide, false, 0, v.wide);
    if (v.operation == Op::MultiplyAdd || v.operation == Op::MultiplySubtract ||
        v.operation == Op::NegatedMultiplyAdd ||
        v.operation == Op::NegatedMultiplySubtract)
        use(v.destination, v.wide, false, 0, v.wide);
}
void ScalarRegisterPlan::Builder::observe(
    const arm::VectorTransferOperands& v, bool load)
{
    const bool scalar = v.mode == arm::VectorTransferMode::ScalarMultiple;
    const auto first = v.first * (scalar ? v.element_size / 4U : 2U);
    if (scalar && v.count == 1) {
        use(first, v.element_size == 8, load, 2);
        return;
    }
    // Bulk and partial vector accesses keep their existing packed lowering.
    // Reserve even untouched lane neighbours; no resident may alias that D.
    const auto words = v.count * (scalar ? v.element_size / 4U : 2U);
    for (unsigned word = first; word < first + words; ++word)
        blocked_[word] = true;
}
ScalarRegisterPlan ScalarRegisterPlan::Builder::finish() const
{
    struct Candidate {
        Entry entry;
        unsigned weight;
        bool split = false, upper_written = false;
    };
    std::array<Candidate, 96> candidates { };
    unsigned candidate_count = 0;
    for (unsigned word = 0; word < singles_.size(); ++word) {
        const auto& single = singles_[word];
        if (single.present && single.weight != 0 && !blocked_[word] &&
            !doubles_[word / 2U].present)
            candidates[candidate_count++] = {
                { word, 0, false, single.written }, single.weight
            };
        if (word % 2U != 0)
            continue;
        const auto& dual = doubles_[word / 2U];
        if (dual.present && !blocked_[word] && !blocked_[word + 1U]) {
            const auto& upper = singles_[word + 1U];
            const bool split = !dual.whole && (single.present || upper.present);
            const auto weight = dual.weight + single.weight + upper.weight;
            if (weight != 0)
                candidates[candidate_count++] = {
                    { word, 0, !split,
                        dual.written || single.written ||
                            (!split && upper.written) },
                    weight / (split ? 2U : 1U), split,
                    dual.written || upper.written
                };
        }
    }
    ScalarRegisterPlan plan;
    unsigned occupied = 0;
    for (unsigned word = 0; word < singles_.size(); ++word)
        if (blocked_[word] || singles_[word].present ||
            doubles_[word / 2U].present)
            occupied |= 1U << (word / 4U);
    // All architectural operands in the entire cycle reserve their packed bank,
    // including overlapping precisions and candidates that cannot be selected.
    const auto available = constant_capacity + 16U -
                           static_cast<unsigned>(std::popcount(occupied));
    std::sort(candidates.begin(), candidates.begin() + candidate_count,
        [](const Candidate& a, const Candidate& b) {
            return a.weight != b.weight ? a.weight > b.weight
                                        : a.entry.word < b.entry.word;
        });
    for (unsigned i = 0; i < candidate_count; ++i) {
        const auto& candidate = candidates[i];
        const auto slots = candidate.split ? 2U : 1U;
        if (plan.count + slots > available)
            continue;
        plan.entries[plan.count++] = candidate.entry;
        if (candidate.split)
            plan.entries[plan.count++] = { candidate.entry.word + 1U, 0, false,
                candidate.upper_written };
    }
    unsigned bank = 0;
    for (unsigned i = 0; i < plan.count; ++i) {
        if (i < constant_capacity)
            plan.entries[i].host = 3U + i;
        else {
            while ((occupied & (1U << bank)) != 0)
                ++bank;
            plan.entries[i].host = 16U + bank++;
        }
    }
    return plan;
}
}
