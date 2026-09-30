// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "kernel/mach_route_policy.hpp"

namespace ilemu::syscall_routes {
MachRouteResolution MachRoutePolicy::resolve(
    const Entry* entry, std::uint32_t number) const
{
    const auto reference = xnu_reference::mach_slot(reference_, number);
    // Public XNU lacks ARM-specific traps and can lag unpublished iOS changes.
    // Existing audited bindings retain precedence over public reserved slots.
    const bool handled = entry && entry->outcome == Outcome::HandlerValidated;
    const bool mig_fallback = entry && entry->outcome == Outcome::MigFallback;
    using xnu_reference::MachSlot;
    const bool invalid = reference == MachSlot::Invalid ||
        reference == MachSlot::Absent ||
        (entry && entry->outcome == Outcome::MachInvalid);
    return { reference, handled ? MachDisposition::HandlerValidated
            : invalid && !mig_fallback ? MachDisposition::ErrnoStub
                                     : MachDisposition::Deferred,
        mig_fallback ? 0x10000003U : 4U, // MACH_SEND_INVALID_DEST / kern_invalid
        !entry || entry->outcome == Outcome::MachUnknown };
}
std::string_view mach_disposition_name(MachDisposition disposition)
{
    switch (disposition) {
    case MachDisposition::HandlerValidated: return "handler-validates";
    case MachDisposition::Deferred: return "deferred";
    case MachDisposition::ErrnoStub: return "errno-stub";
    }
    return "invalid";
}
std::string_view mach_reference_name(xnu_reference::MachSlot slot)
{
    using xnu_reference::MachSlot;
    switch (slot) {
    case MachSlot::UnknownProfile: return "unknown-profile";
    case MachSlot::Call: return "call";
    case MachSlot::FirmwareStub: return "firmware-stub";
    case MachSlot::Invalid: return "kern-invalid";
    case MachSlot::Absent: return "absent";
    }
    return "invalid";
}
} // namespace ilemu::syscall_routes
