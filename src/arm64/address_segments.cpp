/* SPDX-License-Identifier: MPL-2.0 */
#include "address_segments.hpp"
#include <algorithm>
#include <limits>

namespace ilemu::execution::arm64 {
void AddressSegments::observe(unsigned step, const arm::Instruction& inst,
    const std::optional<AddressCache::Key>& key)
{
    if (key && inst.condition == 14 && key->base < 15 && !key->index &&
        !key->pc_base && key->offset <= 4096 && key->size <= 4096) {
        auto found = std::find_if(active_.begin(), active_.end(), [&](unsigned id) {
            const auto& first = groups_[id].uses.front().key;
            return first.base == key->base && first.load == key->load &&
                first.alignment == key->alignment &&
                first.natural_alignment == key->natural_alignment;
        });
        if (found == active_.end()) {
            active_.push_back(static_cast<unsigned>(groups_.size()));
            groups_.emplace_back(groups_.get_allocator().resource());
            found = std::prev(active_.end());
        }
        auto& group = groups_[*found];
        const auto value = group.offsets.expression(key->base).value();
        if (value.index)
            group.stride = value.index;
        const auto offset = key->add ? std::int64_t { key->offset }
                                    : -std::int64_t { key->offset };
        const auto constant = std::int64_t { value.constant } + offset;
        if (constant >= INT32_MIN && constant <= INT32_MAX)
            group.uses.push_back({ step, *key,
                static_cast<std::int32_t>(constant), value.coefficient });
        else
            active_.erase(found);
    }
    for (auto it = active_.begin(); it != active_.end();) {
        auto& group = groups_[*it];
        group.offsets.observe(inst);
        const auto value = group.offsets.expression(group.uses.front().key.base);
        if (!value || (value->index && !group.offsets.invariant(*value->index)) ||
            (group.stride && !group.offsets.invariant(*group.stride)) ||
            (value->index && group.stride && value->index != group.stride))
            it = active_.erase(it);
        else
            ++it;
    }
}
void AddressSegments::finish(unsigned steps)
{
    accesses_.resize(steps);
    for (unsigned id = 0; id < groups_.size(); ++id) {
        const auto& group = groups_[id];
        // Range checks must amortize across several actual accesses.
        if (group.uses.size() < 3)
            continue;
        const auto alignment = std::max(group.uses.front().key.alignment,
            group.uses.front().key.natural_alignment);
        std::int64_t first = INT64_MAX, end = INT64_MIN;
        std::int64_t first_coefficient = INT64_MAX, last_coefficient = INT64_MIN;
        bool valid = true;
        for (auto it = group.uses.rbegin(); it != group.uses.rend(); ++it) {
            first = std::min<std::int64_t>(first, it->constant);
            end = std::max<std::int64_t>(end, std::int64_t { it->constant } + it->key.size);
            first_coefficient = std::min<std::int64_t>(first_coefficient, it->coefficient);
            last_coefficient = std::max<std::int64_t>(last_coefficient, it->coefficient);
            const auto low = first_coefficient - it->coefficient;
            const auto high = last_coefficient - it->coefficient;
            valid &= (std::int64_t { it->constant } - group.uses.front().constant) % alignment == 0 &&
                it->coefficient % static_cast<std::int32_t>(alignment) == 0;
            if (!valid || end - first > 4096 || low < INT32_MIN || high > INT32_MAX)
                continue;
            accesses_[it->step] = Access { id, it->key,
                first - it->constant, end - it->constant,
                static_cast<std::int32_t>(low), static_cast<std::int32_t>(high), group.stride };
        }
    }
    // Only the per-access proof is needed while emitting the native body.
    active_.clear();
    groups_.clear();
}
const AddressSegments::Access* AddressSegments::find(
    unsigned step, const AddressCache::Key& key) const
{
    if (step < accesses_.size() && accesses_[step] && accesses_[step]->key == key)
        return &*accesses_[step];
    return nullptr;
}
void AddressSegments::guard(oaknut::VectorCodeGenerator& code,
    const Access& access, oaknut::Label& checked) const
{
    using namespace oaknut;
    using namespace oaknut::util;
    // W15/W16 retain the original address/writeback payload for a checked exit.
    const auto alignment = std::max(access.key.alignment, access.key.natural_alignment);
    if (alignment > 1) {
        code.AND(W17, W15, alignment - 1U);
        code.CBNZ(W17, checked);
    }
    const auto bound = [&](std::int64_t constant, std::int32_t coefficient) {
        code.AND(W21, W15, 4095);
        if (coefficient != 0) {
            code.MOV(W17, static_cast<std::uint32_t>(coefficient));
            code.SMADDL(X21, WReg { static_cast<int>(*access.stride) }, W17, X21);
        }
        if (constant < 0)
            code.SUB(X21, X21, static_cast<unsigned>(-constant));
        else if (constant != 0)
            code.ADD(X21, X21, static_cast<unsigned>(constant));
        code.LSR(X21, X21, 12);
        code.CBNZ(X21, checked);
    };
    bound(access.first, access.first_coefficient);
    bound(access.end - 1, access.last_coefficient);
    if (access.first_coefficient != access.last_coefficient) {
        // Both signs are bounded without touching live architectural NZCV.
        bound(access.first, access.last_coefficient);
        bound(access.end - 1, access.first_coefficient);
    }
}
}
