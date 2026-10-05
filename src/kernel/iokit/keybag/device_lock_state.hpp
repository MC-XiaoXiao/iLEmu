// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ilemu::kernel_iokit::keybag {

// AppleKeyStore device state for a virtual store with no passcode. The
// single-bag query uses the key/value DER format decoded by MobileKeyBag;
// the extended query uses its packed 42-byte structure.
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

    [[nodiscard]] static std::vector<std::byte> encoded_no_passcode()
    {
        std::vector<std::byte> entries;
        const auto entry = [&entries](const char* key, std::uint8_t key_size,
                               std::uint8_t value) {
            entries.push_back(std::byte { 0x30 });
            entries.push_back(static_cast<std::byte>(key_size + 5U));
            entries.push_back(std::byte { 0x0c });
            entries.push_back(static_cast<std::byte>(key_size));
            for (std::uint8_t i = 0; i < key_size; ++i)
                entries.push_back(static_cast<std::byte>(key[i]));
            entries.push_back(std::byte { 0x02 });
            entries.push_back(std::byte { 0x01 });
            entries.push_back(static_cast<std::byte>(value));
        };
        entry("ss", 2U, 1U << 1U);
        entry("sls", 3U, no_passcode);
        entry("sb", 2U, 0U);
        entry("sfa", 3U, 0U);
        entry("sgs", 3U, unlocked_since_boot);
        entry("sas", 3U, 0U);
        entry("sgpe", 4U, 0U);
        entry("srcd", 4U, 0U);
        entry("sr", 2U, 0U);
        std::vector<std::byte> result;
        result.reserve(entries.size() + 2U);
        // MobileKeyBag decodes a SET of key/value SEQUENCE entries.
        result.push_back(std::byte { 0x31 });
        result.push_back(static_cast<std::byte>(entries.size()));
        result.insert(result.end(), entries.begin(), entries.end());
        return result;
    }
};

} // namespace ilemu::kernel_iokit::keybag
