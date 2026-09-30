// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <cstddef>
#include <span>

namespace ilemu {
// Without a stable writable destination, payload copy is the receive commit
// boundary and the backend retains ownership until the copy succeeds. An
// existing storage lifetime guarantee permits direct receive instead. No
// guest or host address-space details cross this interface.
class SocketReceiveTarget {
public:
    virtual ~SocketReceiveTarget() = default;
    // Optional guarantee: any prefix up to capacity can be copied without
    // fault and its storage stays stable until copy returns. A transient
    // pointer/protection check alone is insufficient. The caller may already
    // own a suitable memory lifetime scope, requiring no new reservation.
    [[nodiscard]] virtual bool can_copy_without_fault(std::size_t) const { return false; }
    [[nodiscard]] virtual bool copy(std::span<const std::byte> bytes) = 0;
};
} // namespace ilemu
