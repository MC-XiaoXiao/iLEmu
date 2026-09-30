// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <cstddef>
#include <span>

namespace ilemu {
// Payload copy is the commit boundary for a socket receive. The backend
// retains ownership until the destination accepts the bytes. No guest or
// host address-space details cross this interface.
class SocketReceiveTarget {
public:
    virtual ~SocketReceiveTarget() = default;
    [[nodiscard]] virtual bool copy(std::span<const std::byte> bytes) = 0;
};
} // namespace ilemu
