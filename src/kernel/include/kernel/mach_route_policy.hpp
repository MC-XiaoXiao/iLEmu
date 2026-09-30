// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "kernel/syscall_routes.hpp"
#include "kernel/xnu_reference_mach_traps.hpp"

namespace ilemu::syscall_routes {
enum class MachDisposition { HandlerValidated, Deferred, ErrnoStub };
struct MachRouteResolution {
    xnu_reference::MachSlot reference;
    MachDisposition disposition;
    std::uint32_t fallback_result;
    bool trace_unknown;
    [[nodiscard]] bool invokes_handler() const
    {
        return disposition == MachDisposition::HandlerValidated;
    }
};
// Configuration-time evidence and routing policy shared by execution and CLI.
// The profile selects a reference table, never an application/build exception.
class MachRoutePolicy {
public:
    explicit MachRoutePolicy(std::string_view abi_profile)
        : reference_ { xnu_reference::mach_table(abi_profile) } { }
    [[nodiscard]] MachRouteResolution resolve(
        const Entry* entry, std::uint32_t number) const;
    [[nodiscard]] std::string_view source() const
    {
        return reference_ ? reference_->source : "unknown-profile";
    }
private:
    const xnu_reference::MachTable* reference_;
};
[[nodiscard]] std::string_view mach_disposition_name(MachDisposition disposition);
[[nodiscard]] std::string_view mach_reference_name(xnu_reference::MachSlot slot);
} // namespace ilemu::syscall_routes
