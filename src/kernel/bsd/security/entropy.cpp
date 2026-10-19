// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/kernel.hpp"

#include "crypto/host_entropy.hpp"

#include "../support.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace ilemu {

void CompatibilityKernel::dispatch_bsd_getentropy(Cpu& cpu)
{
    // XNU randomdev.c accepts at most 256 bytes and copies directly to the
    // caller's buffer. Keep the host entropy source outside the guest kernel.
    std::array<std::byte, 256> bytes { };
    const auto& registers = cpu.registers();
    const auto size = registers[1];
    if (size > bytes.size()) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
    }
    const auto result = std::span { bytes.data(), size };
    if (!fill_host_entropy(result)) {
        bsd_error(cpu, 5U); // EIO if the host entropy provider fails.
        return;
    }
    if (size != 0U && !memory_.copy_to_user(registers[0], result)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
    }
    bsd_success(cpu, 0);
}

} // namespace ilemu
