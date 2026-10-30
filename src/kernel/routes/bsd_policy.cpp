// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "kernel/bsd_route_policy.hpp"

namespace ilemu::syscall_routes {
BsdRouteResolution BsdRoutePolicy::resolve(
    const Entry* entry, std::uint32_t number) const
{
    const auto canonical = entry ? entry->canonical_number : number;
    const auto reference = xnu_reference::bsd_slot(darwin_release_, canonical);
    using xnu_reference::BsdSlot;
    const bool defined = reference == BsdSlot::Call ||
                         reference == BsdSlot::Enosys ||
                         reference == BsdSlot::FirmwareStub;
    // Public desktop reference tables describe fallback results. They cannot
    // disable implemented iOS handlers, including additive calls on old guests.
    const bool candidate = entry && entry->outcome == Outcome::HandlerValidated;
    const auto disposition =
        candidate ? BsdDisposition::HandlerValidated
        : (reference == BsdSlot::Nosys || reference == BsdSlot::Enosys)
            ? BsdDisposition::ErrnoStub
            : BsdDisposition::Deferred;
    return { canonical, reference, disposition, send_sigsys_ && !defined,
        !entry || entry->outcome == Outcome::BsdUnknown };
}

std::string_view bsd_disposition_name(BsdDisposition disposition)
{
    switch (disposition) {
    case BsdDisposition::HandlerValidated:
        return "handler-validates";
    case BsdDisposition::Deferred:
        return "deferred";
    case BsdDisposition::ErrnoStub:
        return "errno-stub";
    }
    return "invalid";
}

std::string_view bsd_reference_name(xnu_reference::BsdSlot slot)
{
    using xnu_reference::BsdSlot;
    switch (slot) {
    case BsdSlot::UnknownRelease:
        return "unknown-release";
    case BsdSlot::Call:
        return "call";
    case BsdSlot::Enosys:
        return "enosys";
    case BsdSlot::FirmwareStub:
        return "firmware-stub";
    case BsdSlot::Nosys:
        return "nosys";
    }
    return "invalid";
}
} // namespace ilemu::syscall_routes
