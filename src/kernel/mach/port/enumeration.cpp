// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//

#include "kernel/kernel.hpp"
#include "queries.hpp"

namespace ilemu {
bool CompatibilityKernel::dispatch_mach_port_query_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    if (!port_mig::Queries::handles(request.identifier))
        return false;
    auto& registers = cpu.registers();
    std::lock_guard lock { shared_state_->mach_mutex };
    const auto status = port_mig::Queries::try_synchronous_locked(memory_,
        *shared_state_, process_, registers, request.bits, request.local_port,
        request.identifier);
    if (!status)
        return false;
    registers[0] = *status;
    return true;
}
} // namespace ilemu
