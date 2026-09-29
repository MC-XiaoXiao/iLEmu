// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ilemu::kernel_iokit::keybag {

// AppleKeyStore's packed device-state wire format. MobileKeyBag uses the
// same 42-byte result for basic and extended queries, while older clients
// request a scalar. The restored virtual key store has no passcode.
class DeviceLockState {
public:
    static constexpr std::uint32_t packed_size = 42U;
    static constexpr std::uint32_t no_passcode = 3U;
    static constexpr std::uint32_t unlocked_since_boot = 1U << 2U;

    [[nodiscard]] static std::vector<std::byte> packed_no_passcode()
    {
        std::vector<std::byte> bytes(packed_size);
        const auto word = [&](std::size_t offset, std::uint32_t value) {
            for (unsigned i = 0; i != 4; ++i)
                bytes[offset + i] = static_cast<std::byte>(value >> (8U * i));
        };
        // get_device_state_internal in the original driver derives state 3
        // from flags bit 1. Timers, failed attempts and assertion flags are
        // zero for this store; offset 20 carries the bag's lock-state bits.
        word(0, 1U << 1U);
        word(4, no_passcode);
        word(20, unlocked_since_boot);
        return bytes;
    }
};

} // namespace ilemu::kernel_iokit::keybag
