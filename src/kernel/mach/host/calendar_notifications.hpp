// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "kernel/kernel_shared_state.hpp"
#include "../support.hpp"

#include <cstdint>

namespace ilemu::host_mig {

// XNU host_notify.c's one-shot calendar change/set registrations. Callers
// hold mach_mutex, as for all other shared Mach port state.
class CalendarNotifications {
public:
    static constexpr std::uint32_t calendar_change = 0U;
    static constexpr std::uint32_t calendar_set = 1U;

    static std::uint32_t register_locked(KernelSharedState& state,
        std::uint32_t process_id, std::uint32_t name, std::uint32_t type);
    static void clock_set_locked(KernelSharedState& state);
    static void port_destroyed_locked(
        KernelSharedState& state, std::uint32_t object);
};

} // namespace ilemu::host_mig
