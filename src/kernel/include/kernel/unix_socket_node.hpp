// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "foundation/darwin_errno.hpp"
#include <cstdint>
#include <limits>

namespace ilemu {

// A bound AF_UNIX pathname has vnode permissions even though transport is
// entirely guest-local. Callers hold KernelSharedState::socket_mutex.
class UnixSocketNode {
public:
    UnixSocketNode(std::uint32_t owner, std::uint32_t group, std::uint32_t mode)
        : owner_ { owner }, group_ { group }, mode_ { mode & 07777U } { }

    std::uint32_t change_owner(std::uint32_t caller_uid, std::uint32_t caller_gid,
        std::uint32_t owner, std::uint32_t group)
    {
        constexpr auto unchanged = std::numeric_limits<std::uint32_t>::max();
        if (caller_uid != 0U &&
            (caller_uid != owner_ || (owner != unchanged && owner != owner_) ||
                (group != unchanged && group != group_ && group != caller_gid)))
            return darwin::error::operation_not_permitted;
        if (owner != unchanged)
            owner_ = owner;
        if (group != unchanged)
            group_ = group;
        return 0;
    }

    std::uint32_t change_mode(std::uint32_t caller_uid, std::uint32_t mode)
    {
        if (caller_uid != 0U && caller_uid != owner_)
            return darwin::error::operation_not_permitted;
        mode_ = mode & 07777U;
        return 0;
    }

private:
    std::uint32_t owner_;
    std::uint32_t group_;
    std::uint32_t mode_;
};

} // namespace ilemu
