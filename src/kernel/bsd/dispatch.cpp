// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// BSD routes are resolved once into the session dispatch table.
#include "kernel/darwin_abi.hpp"
#include "kernel/kernel.hpp"
#include "support.hpp"

namespace ilemu {
void CompatibilityKernel::dispatch_bsd_nosys(Cpu& cpu, bool send_sigsys)
{
    // Publish errno/carry before signal delivery can terminate the caller.
    bsd_error(cpu, bsd_support::not_implemented);
    if (!send_sigsys)
        return;
    static_cast<void>(deliver_signal(darwin::signal::bad_system_call));
    if (process_.exited)
        cpu.halt(Dynarmic::HaltReason::UserDefined1);
}

void CompatibilityKernel::dispatch_bsd(Cpu& cpu, std::uint32_t number)
{
    shared_state_->bsd_dispatch_table.dispatch(*this, cpu, number);
}
} // namespace ilemu
