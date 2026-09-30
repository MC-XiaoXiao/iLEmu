// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"

#include <algorithm>
#include <limits>

namespace ilemu {

std::optional<AddressSpace::SharedMapping> AddressSpace::share_mapping(
    std::uint32_t address, std::uint32_t size)
{
    if (size == 0 || address % page_size != 0 || size % page_size != 0 ||
        size - 1U > UINT32_MAX - address)
        return std::nullopt;

    auto lock = write_lock();
    const auto end = std::uint64_t { address } + size;
    SharedMapping result;
    for (std::uint64_t cursor = address; cursor < end;) {
        auto region = vm_map_.region_at_or_after(
            static_cast<std::uint32_t>(cursor));
        if (!region || region->address > cursor)
            return std::nullopt;
        const auto region_end = std::min(region->end, end);
        region->address = static_cast<std::uint32_t>(cursor - address);
        region->end = region_end - address;
        result.permissions = static_cast<MemoryPermission>(
            static_cast<unsigned>(result.permissions) &
            static_cast<unsigned>(region->permissions));
        result.maximum_permissions = static_cast<MemoryPermission>(
            static_cast<unsigned>(result.maximum_permissions) &
            static_cast<unsigned>(region->maximum_permissions));
        result.regions.push_back(*region);
        cursor = region_end;
    }
    // Reject a hole before sharing any pages or changing COW metadata.
    result.pages.reserve(size / page_size);
    share_pages_locked(address, end, &result.pages);
    return result;
}

bool AddressSpace::map_shared_mapping(std::uint32_t address,
    const SharedMapping& mapping, PageMappingMode mode,
    VmInheritance inheritance)
{
    if (mapping.pages.size() > UINT32_MAX / page_size)
        return false;
    return map_page_ranges(address,
        static_cast<std::uint32_t>(mapping.pages.size() * page_size),
        mapping.pages, mapping.regions, mode, inheritance, nullptr);
}

} // namespace ilemu
