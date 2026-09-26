// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <compare>
#include <cstdint>
#include <memory>

namespace ilemu {

// A byte in a page backing or live file object. Ownership ordering uses the control
// block, not its recycled host address or a write-invalidated reservation.
// Weak ownership neither pins page bytes nor changes guest COW decisions.
struct SharedMemoryIdentity {
    std::weak_ptr<const void> backing;
    std::uint64_t offset { };

    [[nodiscard]] std::strong_ordering operator<=>(
        const SharedMemoryIdentity& other) const noexcept
    {
        if (backing.owner_before(other.backing))
            return std::strong_ordering::less;
        if (other.backing.owner_before(backing))
            return std::strong_ordering::greater;
        return offset <=> other.offset;
    }

    [[nodiscard]] bool operator==(
        const SharedMemoryIdentity& other) const noexcept
    {
        return (*this <=> other) == 0;
    }
};

} // namespace ilemu
