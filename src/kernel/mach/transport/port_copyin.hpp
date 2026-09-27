// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "../support.hpp"

namespace ilemu::mach_transport {

// Resolve a message capability under mach_mutex without consuming its uref.
// XNU ipc_right_copyin accepts dead names for COPY/MOVE_SEND and
// MOVE_SEND_ONCE; MAKE and MOVE_RECEIVE require a live right.
class PortCopyin {
public:
    using Transfer = KernelSharedState::MachMessage::PortTransfer;

    PortCopyin(const KernelSharedState& state, std::uint32_t task)
        : state_(state), task_(task)
    {
    }

    [[nodiscard]] std::optional<Transfer> capture(std::uint32_t name,
        std::uint32_t disposition, std::uint32_t offset,
        std::optional<std::uint32_t> array_index = { }) const
    {
        const auto source =
            mach_support::source_right_for_disposition(disposition);
        const auto right = mach_support::right_for_disposition(disposition);
        if (!source || !right)
            return std::nullopt;
        const auto entry = state_.mach_namespaces.lookup(task_, name);
        if (!entry)
            return std::nullopt;
        if ((entry->type & xnu::ipc::type_mask(*source)) != 0U) {
            return Transfer { offset, name, array_index, entry->object,
                *right, disposition };
        }
        if (disposition >= 17U && disposition <= 19U &&
            (entry->type &
                xnu::ipc::type_mask(xnu::ipc::Right::DeadName)) != 0U) {
            // A pending sender uref operation, never a queued right.
            return Transfer { offset, name, array_index, xnu::ipc::dead_name,
                xnu::ipc::Right::DeadName, disposition };
        }
        return std::nullopt;
    }

private:
    const KernelSharedState& state_;
    std::uint32_t task_;
};

} // namespace ilemu::mach_transport
