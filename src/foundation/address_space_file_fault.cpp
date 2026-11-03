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
    constexpr std::uint64_t cluster_bytes = guest_file_prefetch_pages * page_size;
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
        const auto begin = std::max(mapping.file_offset, offset & ~(cluster_bytes - 1U));
        const auto finish = std::min({ mapping.backing->end_offset,
            mapping.file_offset + mapping.end - entry->first, begin + cluster_bytes });
        bool needs_read = false;
        // The ordinary fault path publishes this whole cluster. Check its
        // tail too when preparation or an existing alias seeded only a prefix.
        for (auto file_page = begin; file_page < finish; file_page += page_size) {
            const auto count = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(page_size, finish - file_page));
            if (mapping.cache->needs_file_read(mapping.backing, file_page, count, shared)) {
                needs_read = true;
                break;
            }
        }
        if (!needs_read)
            continue;
        auto read = file_pager_->prepare(mapping.cache, mapping.backing,
            begin, static_cast<std::size_t>(finish - begin), shared);
        FilePageFaultScope::suspend({ std::move(read), address, size, access });
        return false;
    }
    return true;
}
}
