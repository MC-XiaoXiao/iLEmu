// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include <algorithm>
#include <limits>

namespace ilemu {
AddressSpace::ProtectResult AddressSpace::protect_with_result(
    std::uint32_t address, std::uint32_t size, MemoryPermission permissions,
    bool set_maximum, bool copy)
{
    if (size == 0 || size - 1U > UINT32_MAX - address)
        return { .succeeded = size == 0 };
    constexpr auto mask = std::uint64_t { page_size - 1U };
    const auto first = address & ~static_cast<std::uint32_t>(mask);
    const auto end = (std::uint64_t { address } + size + mask) & ~mask;
    auto lock = write_lock();
    const auto result = vm_map_.protect(first, end, permissions, set_maximum, copy);
    if (result.error != VmMap::ProtectionError::None)
        return { .protection_failure =
            result.error == VmMap::ProtectionError::ProtectionFailure };
    if (copy)
        privatize_range_locked(first, end);
    // Maximum changes intersect current permissions separately for each entry.
    for (std::uint64_t cursor = first; cursor < end;) {
        const auto region = vm_map_.region_at_or_after(static_cast<std::uint32_t>(cursor));
        const auto region_end = std::min(region->end, end);
        set_page_permissions_locked(static_cast<std::uint32_t>(cursor),
            region_end, region->permissions);
        if (has_permission(region->permissions, MemoryPermission::Write)) {
            ensure_unique_page_map_locked();
            for (auto page = pages_->lower_bound(static_cast<std::uint32_t>(cursor));
                page != pages_->end() && page->first < region_end; ++page) {
                if (page->second.file_writeback_capable)
                    page->second.file_writeback = true;
            }
        }
        cursor = region_end;
    }
    refresh_jit_page_range_locked(first, end);
    if (result.executable_permissions_changed)
        bump_executable_content_generation_locked();
    return { .succeeded = true,
        .executable_permissions_changed = result.executable_permissions_changed };
}

bool AddressSpace::protect(
    std::uint32_t address, std::uint32_t size, MemoryPermission permissions)
{
    return protect_with_result(address, size, permissions).succeeded;
}

void AddressSpace::privatize_range_locked(std::uint32_t address, std::uint64_t end)
{
    // Keep demand paging lazy. Split file intervals without duplicating their
    // retained file descriptors or changing neighbouring shared mappings.
    const auto split = [&](std::uint64_t point) {
        if (point >= (std::uint64_t { 1 } << 32U))
            return;
        auto after = file_mappings_.upper_bound(static_cast<std::uint32_t>(point));
        if (after == file_mappings_.begin())
            return;
        auto entry = std::prev(after);
        if (point <= entry->first || point >= entry->second.end)
            return;
        auto right = entry->second;
        right.file_offset += point - entry->first;
        entry->second.end = point;
        file_mappings_.emplace(static_cast<std::uint32_t>(point), std::move(right));
    };
    split(address);
    split(end);
    for (auto entry = file_mappings_.lower_bound(address);
        entry != file_mappings_.end() && entry->first < end; ++entry)
        entry->second.needs_copy = true;

    ensure_unique_page_map_locked();
    // Publish previous shared writes before removing their writeback owner.
    // Subsequent writes use the same private-backing detach path as fork.
    static_cast<void>(flush_shared_file_pages_locked(address, end));
    for (auto entry = pages_->lower_bound(address);
        entry != pages_->end() && entry->first < end; ++entry) {
        auto& page = entry->second;
        page.shared_writable = false;
        page.copy_on_write_possible = true;
        page.file_cached |= page.backing && page.backing->file_backed();
        page.file_writeback = false;
        page.file_writeback_capable = false;
    }
}

} // namespace ilemu
