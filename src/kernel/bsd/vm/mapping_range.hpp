// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "foundation/address_space.hpp"
#include <cstdint>
#include <optional>

namespace ilemu::bsd_vm {

// kern_mman.c:mmap (XNU792 through XNU3248): the returned byte address
// retains the file page offset, while VM and pager operations use whole pages.
struct MappingRange {
    std::uint32_t address;
    std::uint32_t size;
    std::uint64_t file_offset;
    std::uint32_t page_offset;

    static constexpr std::optional<MappingRange> from_request(
        std::uint32_t address, std::uint32_t size, std::uint64_t offset, bool fixed)
    {
        constexpr auto mask = std::uint64_t { AddressSpace::page_size - 1U };
        const auto page_offset = static_cast<std::uint32_t>(offset & mask);
        const auto extent = (std::uint64_t { size } + page_offset + mask) & ~mask;
        if (size == 0U || extent > UINT32_MAX ||
            offset > (UINT64_MAX & ~mask) - size)
            return std::nullopt;
        if (fixed) {
            // Validate before replacing an existing mapping. Both offsets
            // must match; rounding an invalid request down would destroy it.
            if (address < page_offset || ((address - page_offset) & mask) != 0U)
                return std::nullopt;
            address -= page_offset;
        } else {
            const auto hint = (std::uint64_t { address } + mask) & ~mask;
            address = hint <= UINT32_MAX ? static_cast<std::uint32_t>(hint) : 0U;
        }
        return MappingRange { address, static_cast<std::uint32_t>(extent),
            offset - page_offset, page_offset };
    }
};

} // namespace ilemu::bsd_vm
