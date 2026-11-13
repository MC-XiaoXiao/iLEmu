/* SPDX-License-Identifier: MPL-2.0 */
// XNU3789 bsd/kern/kern_csr.c: a normal boot without a CSR override has
// active_config=0. Queries report the guest boot policy, not the host's policy.
#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"
#include "../support.hpp"

namespace ilemu {

void CompatibilityKernel::dispatch_bsd_csr(Cpu& cpu)
{
    const auto& registers = cpu.registers();
    const auto operation = registers[0];
    if (operation > 1U) {
        bsd_error(cpu, bsd_support::not_implemented);
        return;
    }
    if (registers[1] == 0U || registers[2] != 4U) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
    }
    if (operation == 0U) {
        const auto mask = memory_.read32(registers[1]);
        if (!mask)
            bsd_error(cpu, darwin::error::bad_address);
        else
            bsd_error(cpu, *mask == 0U ? UINT32_MAX : darwin::error::operation_not_permitted);
        return;
    }
    const std::array<std::byte, 4> configuration { };
    if (!memory_.copy_to_user(registers[1], configuration))
        bsd_error(cpu, darwin::error::bad_address);
    else
        bsd_success(cpu, 0U);
}

} // namespace ilemu
