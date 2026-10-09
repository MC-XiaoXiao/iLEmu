/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <array>
#include <cstdint>
#include <oaknut/oaknut.hpp>
#include <optional>

namespace ilemu::execution::arm64 {
// Dominating unconditional guards publish into X27/X28. Every trace-head
// traversal initializes these pointers again; no pointer survives a native
// invocation or a checked memory exit. Callers revoke entries on base writes.
class AddressCache {
public:
    struct Key {
        unsigned base, size;
        std::uint32_t offset;
        bool add, load;
        unsigned alignment, natural_alignment;
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
    oaknut::XReg remember(const Key& key, oaknut::XReg pointer)
    {
        const auto slot = next_++ % entries_.size();
        entries_[slot] = key;
        const auto reg = oaknut::XReg { 27 + static_cast<int>(slot) };
        code_.MOV(reg, pointer);
        return reg;
    }
    void invalidate(unsigned base)
    {
        for (auto& entry : entries_)
            if (entry && entry->base == base)
                entry.reset();
    }
    void clear() { entries_ = { }; }

private:
    oaknut::VectorCodeGenerator& code_;
    std::array<std::optional<Key>, 2> entries_;
    unsigned next_ = 0;
};
}
