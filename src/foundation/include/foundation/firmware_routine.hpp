// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ilemu {

class AddressSpace;

// Identify the complete guest implementation, including any inline constants.
// Instruction-fetch overlays leave the bytes observed by read_bytes intact.
struct FirmwareRoutine {
    std::size_t size;
    std::array<std::uint8_t, 32> digest;

    [[nodiscard]] bool matches(AddressSpace& memory, std::uint32_t entry) const;
};

} // namespace ilemu
