// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ilemu {

// Embedded cache-local nlist32 tables, indexed by the Mach-O header's file
// offset. Keep the container ABI separate from Mach-O symbol decoding.
struct Arm32DyldCacheLocalSymbols {
    std::span<const std::byte> entries;
    std::span<const std::byte> strings;

    static std::optional<Arm32DyldCacheLocalSymbols> find(
        std::span<const std::byte> cache, std::uint64_t image_offset);
};

} // namespace ilemu
