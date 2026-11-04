// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <memory>

namespace ilemu {
class AddressSpace;
class FilePageTransform;

// Retain the firmware's v2 metadata for demand-faulted private file pages.
// Delta bits must be decoded even at slide zero. Invalid metadata returns null.
[[nodiscard]] std::shared_ptr<const FilePageTransform>
prepare_shared_region_slide_v2(const AddressSpace& memory,
    std::uint32_t mapping_size, std::uint64_t file_offset, std::uint32_t slide,
    std::uint32_t slide_info_address, std::uint32_t slide_info_size);
} // namespace ilemu
