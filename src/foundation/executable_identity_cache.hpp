// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "foundation/address_space.hpp"

namespace ilemu {

// Memoize only the pure digest calculation. The caller must validate the live
// mapping and page eligibility before every lookup. No Guest or backing pointer
// is retained, so entries can be reused across independently validated spaces.
class ExecutableIdentityCache {
public:
    [[nodiscard]] ExecutableBackingIdentity get(const ContentIdentity& source,
        std::uint64_t page, std::uint64_t mapping_end, std::uint64_t file_offset)
    {
        const Key key { source, page, mapping_end, file_offset };
        auto& entry = entries_[(page / AddressSpace::page_size) % entries_.size()];
        if (entry && entry->key == key)
            return entry->identity;

        std::array<std::byte, 3U * sizeof(std::uint64_t) +
                                  ContentIdentity { }.digest.size()> layout { };
        const std::array fields { page, mapping_end, file_offset };
        for (std::size_t index = 0; index < fields.size(); ++index) {
            for (unsigned byte = 0; byte < sizeof(std::uint64_t); ++byte) {
                layout[index * sizeof(std::uint64_t) + byte] =
                    static_cast<std::byte>(fields[index] >> (byte * 8U));
            }
        }
        std::copy(source.digest.begin(), source.digest.end(),
            layout.begin() + 3U * sizeof(std::uint64_t));
        const ExecutableBackingIdentity identity {
            sha256(source.digest), sha256(layout) };
        entry = Entry { key, identity };
        return identity;
    }

private:
    struct Key {
        ContentIdentity source;
        std::uint64_t page;
        std::uint64_t mapping_end;
        std::uint64_t file_offset;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct Entry {
        Key key;
        ExecutableBackingIdentity identity;
    };
    // Direct-mapped, bounded storage: collisions replace an entry and never
    // substitute for the complete key comparison. No allocation on lookup.
    std::array<std::optional<Entry>, 64U> entries_ { };
};

static_assert(sizeof(ExecutableIdentityCache) <= 8U * 1024U);

} // namespace ilemu
