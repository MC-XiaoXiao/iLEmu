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
void ScalarRegisterPlan::Builder::use(unsigned word, bool wide, bool write)
{
    auto& value = wide ? doubles_[word / 2U] : singles_[word];
    value.present = true;
    value.written |= write;
    // Writes remove packed-bank dependencies; upper reads remove extraction.
    value.weight += write ? 8U : (word % 4U != 0 ? 2U : 0U);
}
void ScalarRegisterPlan::Builder::observe(
    const arm::FloatingArithmeticOperands& v)
{
    using Op = arm::FloatingArithmeticOperation;
    use(v.destination, v.wide, true);
    use(v.first, v.wide, false);
    if (v.operation != Op::Move && v.operation != Op::Absolute &&
        v.operation != Op::Negate && v.operation != Op::SquareRoot)
        use(v.second, v.wide, false);
    if (v.operation == Op::MultiplyAdd || v.operation == Op::MultiplySubtract ||
        v.operation == Op::NegatedMultiplyAdd ||
        v.operation == Op::NegatedMultiplySubtract)
        use(v.destination, v.wide, false);
}
ScalarRegisterPlan ScalarRegisterPlan::Builder::finish() const
{
    struct Candidate {
        Entry entry;
        unsigned weight;
    };
    std::array<Candidate, 96> candidates { };
    unsigned candidate_count = 0;
    for (unsigned word = 0; word < singles_.size(); ++word) {
        const auto& single = singles_[word];
        if (single.present && single.weight != 0 &&
            !doubles_[word / 2U].present)
            candidates[candidate_count++] = { { word, 0, false, single.written },
                single.weight };
        if (word % 2U != 0)
            continue;
        const auto& dual = doubles_[word / 2U];
        if (dual.present && dual.weight != 0 && !single.present &&
            !singles_[word + 1U].present)
            candidates[candidate_count++] = { { word, 0, true, dual.written },
                dual.weight };
    }
    ScalarRegisterPlan plan;
    unsigned occupied = 0;
    for (unsigned word = 0; word < singles_.size(); ++word)
        if (singles_[word].present || doubles_[word / 2U].present)
            occupied |= 1U << (word / 4U);
    // All architectural operands in the entire cycle reserve their packed bank,
    // including overlapping precisions and candidates that cannot be selected.
    const auto available = constant_capacity + 16U - static_cast<unsigned>(std::popcount(occupied));
    plan.count = std::min(candidate_count, available);
    std::partial_sort(candidates.begin(), candidates.begin() + plan.count,
        candidates.begin() + candidate_count, [](const Candidate& a, const Candidate& b) {
            return a.weight != b.weight ? a.weight > b.weight
                                        : a.entry.word < b.entry.word;
        });
    unsigned bank = 0;
    for (unsigned i = 0; i < plan.count; ++i) {
        auto entry = candidates[i].entry;
        if (i < constant_capacity)
            entry.host = 3U + i;
        else {
            while ((occupied & (1U << bank)) != 0)
                ++bank;
            entry.host = 16U + bank++;
        }
        plan.entries[i] = entry;
    }
    return plan;
}
}
