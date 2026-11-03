/* SPDX-License-Identifier: MPL-2.0 */
#include "foundation/address_space.hpp"
#include "foundation/host_file_pager.hpp"
#include <algorithm>

namespace ilemu {
bool AddressSpace::prepare_file_access(std::uint32_t address,
    std::size_t size, MemoryPermission access)
{
    if (!file_pager_ || !FilePageFaultScope::enabled() || size == 0 ||
        size > (std::uint64_t { 1 } << 32U) - address)
        return true;
    auto lock = read_lock();
    // Preserve the ordinary protection-fault contract without reading a file.
    if (!range_accessible_locked(address, size, access))
        return true;
    constexpr std::uint64_t batch_bytes =
        guest_file_read_batch_pages * page_size;
    const auto end = std::uint64_t { address } + size;
    for (std::uint64_t base = address & ~(page_size - 1U); base < end; base += page_size) {
        const auto current = static_cast<std::uint32_t>(base);
        if (const auto* page = find_page_locked(current); page && page->backing)
            continue;
        const auto after = file_mappings_.upper_bound(current);
        if (after == file_mappings_.begin())
            continue;
        const auto entry = std::prev(after);
        const auto& mapping = entry->second;
        if (current >= mapping.end)
            continue;
        const auto offset = mapping.file_offset + base - entry->first;
        const auto shared = mapping.mode != PageMappingMode::CopyOnWrite;
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            page_size, mapping.backing->end_offset - offset));
        if (!mapping.cache->needs_file_read(
                mapping.backing, offset, count, shared))
            continue;
        // Only batch pages touched by this access, within one mapping and the
        // bounded host ticket. Other pages remain lazy until guest demand.
        const auto finish = std::min({ mapping.backing->end_offset,
            mapping.file_offset + mapping.end - entry->first,
            mapping.file_offset +
                ((end + page_size - 1U) & ~(std::uint64_t { page_size } - 1U)) -
                entry->first,
            offset + batch_bytes });
        auto read = file_pager_->prepare(mapping.cache, mapping.backing, offset,
            static_cast<std::size_t>(finish - offset), shared);
        FilePageFaultScope::suspend({ std::move(read), address, size, access });
        return false;
    }
    return true;
}
}
