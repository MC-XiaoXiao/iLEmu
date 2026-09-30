// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
#include "kernel/kernel.hpp"
#include "kernel/kernel_shared_state.hpp"

namespace ilemu {
void CompatibilityKernel::dispatch_mach(Cpu& cpu, std::uint32_t trap)
{
    shared_state_->mach_dispatch_table.dispatch(*this, cpu, trap);
}
} // namespace ilemu
