/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <oaknut/oaknut.hpp>
#include <optional>
#include <vector>

namespace ilemu::execution::arm64 {
// Dominating unconditional guards publish into X27/X28. Only proven invariant
// slots survive a closed trace backedge; no pointer survives a native
// invocation or checked exit. Callers revoke entries on every architectural
// address input write.
class AddressCache {
public:
    struct Key {
        unsigned base, size;
        std::uint32_t offset;
        bool add, load;
        unsigned alignment, natural_alignment;
        std::optional<unsigned> index = { };
        arm::ShiftKind shift = arm::ShiftKind::Lsl;
        unsigned shift_amount = 0;
        std::uint32_t inputs() const
        {
            return (1U << base) | (index ? 1U << *index : 0U);
        }
        bool operator==(const Key&) const = default;
    };
    explicit AddressCache(oaknut::VectorCodeGenerator& code)
        : code_(code)
    {
    }
    std::optional<oaknut::XReg> find(const Key& key) const
    {
        for (unsigned slot = 0; slot < entries_.size(); ++slot)
            if (entries_[slot] == key)
                return oaknut::XReg { 27 + static_cast<int>(slot) };
        return { };
    }
    void begin_guard(const Key& key)
    {
        const auto slot = next_ % entries_.size();
        if (!first_[slot]) {
            first_[slot] = Guard { key, code_.offset(), 0 };
            code_.NOP();
        }
    }
    oaknut::XReg remember(const Key& key, oaknut::XReg pointer)
    {
        const auto slot = next_++ % entries_.size();
        entries_[slot] = key;
        const auto reg = oaknut::XReg { 27 + static_cast<int>(slot) };
        code_.MOV(reg, pointer);
        if (auto& first = first_[slot];
            first && first->target == 0 && first->key == key)
            first->target = code_.offset();
        return reg;
    }
    void invalidate(unsigned reg)
    {
        // Track writes even before the first guard or after slot eviction.
        // Restoring a previous input value is deliberately not analyzed.
        written_ |= 1U << reg;
        for (auto& entry : entries_)
            if (entry && (entry->inputs() & (1U << reg)) != 0)
                entry.reset();
    }
    void clear()
    {
        entries_ = { };
        written_ = 0xffffU;
    }
    void finalize(bool closed, std::vector<std::uint32_t>& words,
        oaknut::VectorCodeGenerator& entry) const
    {
        if (!closed)
            return;
        for (unsigned slot = 0; slot < first_.size(); ++slot) {
            const auto& first = first_[slot];
            if (!first || first->target == 0 || entries_[slot] != first->key ||
                (written_ & first->key.inputs()) != 0)
                continue;
            const oaknut::XReg reg { 27 + static_cast<int>(slot) };
            // The first access still runs its guard at its original PC. Only
            // later traversals reuse the leased pointer. All body offsets move
            // together when the compiler inserts this entry initialization.
            entry.MOV(reg, 0);
            oaknut::CodeGenerator patch(words.data() + first->branch / 4);
            patch.CBNZ(reg, first->target - first->branch);
        }
    }

private:
    struct Guard {
        Key key;
        std::ptrdiff_t branch, target;
    };
    oaknut::VectorCodeGenerator& code_;
    std::array<std::optional<Key>, 2> entries_;
    std::array<std::optional<Guard>, 2> first_;
    unsigned next_ = 0;
    unsigned written_ = 0;
};
}
