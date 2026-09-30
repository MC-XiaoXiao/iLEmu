// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "../support.hpp"

namespace ilemu::mach_transport {
// ipc_kmsg_send invokes ipc_kobject_server only for ipc_space_kernel.
// Message IDs and payload shapes cannot identify a destination service.
// Reference: xnu-2422.115.4/osfmk/ipc/ipc_kmsg.c, ipc_kmsg_send.
class KernelDestination {
public:
    // Caller holds mach_mutex. This is a read-only classification; ordinary
    // transport remains responsible for consuming rights and reporting errors.
    static bool matches_locked(const KernelSharedState& state,
        std::uint32_t pid, std::uint32_t name, std::uint32_t bits)
    {
        using namespace mach_support;
        const auto right = right_for_disposition(bits & 0xffU);
        const auto source = source_right_for_disposition(bits & 0xffU);
        if (!right || !source ||
            (*right != xnu::ipc::Right::Send &&
                *right != xnu::ipc::Right::SendOnce))
            return false;
        auto object = resolve_name_with_right(state, pid, name, *source);
        // Preserve the established received-header compatibility in the send
        // transport: MAKE_SEND may name an already copied-out Send right.
        if (!object && source != right)
            object = resolve_name_with_right(state, pid, name, *right);
        if (!object)
            return false;
        const auto port = state.mach_port_objects.lookup(*object);
        return port && port->kernel_owned;
    }
};
} // namespace ilemu::mach_transport
