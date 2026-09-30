// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "kernel/syscall_routes.hpp"
#include "kernel/xnu_reference_syscalls.hpp"

namespace ilemu::syscall_routes {
// Reaching a handler does not certify all of its commands or argument layouts.
enum class BsdDisposition { HandlerValidated, Deferred, ErrnoStub };
struct BsdRouteResolution {
    std::uint32_t canonical;
    xnu_reference::BsdSlot reference;
    BsdDisposition disposition;
    bool send_sigsys;
    bool trace_unknown;
    bool reference_gated;

    [[nodiscard]] bool invokes_handler() const
    {
        return disposition == BsdDisposition::HandlerValidated;
    }
};

// Resolve only at configuration time. The executable table caches this result;
// no reference lookup or policy branch is added to the syscall hot path.
class BsdRoutePolicy {
public:
    BsdRoutePolicy(const DarwinAbi& abi, std::string_view darwin_release)
        : darwin_release_ { darwin_release },
          send_sigsys_ { abi.capabilities.send_sigsys }
    {
    }
    [[nodiscard]] BsdRouteResolution resolve(
        const Entry* entry, std::uint32_t number) const;

private:
    std::string_view darwin_release_;
    bool send_sigsys_;
};

[[nodiscard]] std::string_view bsd_disposition_name(BsdDisposition disposition);
[[nodiscard]] std::string_view bsd_reference_name(xnu_reference::BsdSlot slot);
} // namespace ilemu::syscall_routes
