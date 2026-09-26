// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Darwin 14 NECP application policy matching. The compatibility kernel does
// not install NECP policies, so a valid query returns XNU's no-match result.
// ABI: xnu-2782.1.97 bsd/net/necp.h and bsd/net/necp.c.
#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"
#include "support.hpp"

namespace ilemu {

void CompatibilityKernel::dispatch_bsd_network_policy(Cpu& cpu)
{
    const auto& registers = cpu.registers();
    constexpr std::uint32_t maximum_parameters = 1024U;
    constexpr std::uint32_t aggregate_result_words = 10U;
    const auto parameters = registers[0];
    const auto parameter_size = registers[1];
    const auto result = registers[2];
    if (parameters == 0U || parameter_size == 0U ||
        parameter_size > maximum_parameters || result == 0U) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
    }
    if (!memory_.read_bytes(parameters, parameter_size)) {
        bsd_error(cpu, darwin::error::bad_address);
        return;
    }
    for (std::uint32_t word = 0; word < aggregate_result_words; ++word) {
        if (!memory_.write32(result + word * sizeof(std::uint32_t), 0U)) {
            bsd_error(cpu, darwin::error::bad_address);
            return;
        }
    }
    bsd_success(cpu, 0U);
}

} // namespace ilemu
