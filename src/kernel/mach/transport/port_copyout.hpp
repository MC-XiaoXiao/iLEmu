// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "../support.hpp"

namespace ilemu::mach_transport {
// Hold mach_mutex; callers release or destroy their captured rights.
class PortCopyout {
public:
    PortCopyout(KernelSharedState& state, std::uint32_t task)
        : state_(state), task_(task) { }
    [[nodiscard]] std::optional<std::uint32_t> operator()(
        std::uint32_t object, xnu::ipc::Right right) const
    {
        if (object == xnu::ipc::null_name)
            return std::nullopt;
        if (!state_.mach_port_objects.contains(object)) {
            if (right != xnu::ipc::Right::Send &&
                right != xnu::ipc::Right::SendOnce) {
                return std::nullopt;
            }
            // Directly retired legacy objects may not have gone through
            // terminate_receive_object_locked. Normalize any stale namespace
            // send entries before returning the sentinel so a PID/name reuse
            // cannot observe a resurrected Send right.
            static_cast<void>(
                state_.mach_namespaces.mark_object_dead(object));
            return xnu::ipc::dead_name;
        }
        return state_.mach_namespaces.copyout(
            task_, object, xnu::ipc::type_mask(right));
    }
private:
    KernelSharedState& state_;
    std::uint32_t task_;
};
} // namespace ilemu::mach_transport
