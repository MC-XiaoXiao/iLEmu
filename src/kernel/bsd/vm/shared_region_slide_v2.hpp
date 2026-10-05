// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>

namespace ilemu {
class AddressSpace;

// Decode the firmware's version-2 dyld cache pointer chains in the writable
// shared-region mapping.  The encoded delta bits must be removed even when
// the requested address slide is zero.
[[nodiscard]] std::uint32_t apply_shared_region_slide_v2(AddressSpace& memory,
    std::uint32_t mapping_address, std::uint32_t mapping_size,
    std::uint32_t initial_protection, std::uint32_t slide,
    std::uint32_t slide_info_address, std::uint32_t slide_info_size);
} // namespace ilemu
