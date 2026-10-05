// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"

#include <algorithm>
#include <limits>
#include <map>

namespace ilemu {

bool AddressSpace::overlay_instructions(
    std::span<const CopyInOperation> operations)
{
    if (operations.empty())
        return true;
    auto lock = write_lock();
    using Bytes = std::array<std::byte, page_size>;
    std::map<std::uint32_t, std::shared_ptr<Bytes>> prepared;
    // Preflight and prepare all pages before publishing any replacement.
    for (const auto& operation : operations) {
        if (operation.data.empty())
            continue;
        const auto end = static_cast<std::uint64_t>(operation.address) +
                         operation.data.size();
        if (end > std::uint64_t { std::numeric_limits<std::uint32_t>::max() } +
                      1U ||
            !range_accessible_locked(operation.address, operation.data.size(),
                MemoryPermission::Execute))
            return false;
        for (std::uint64_t current = operation.address; current < end;) {
            const auto base =
                static_cast<std::uint32_t>(current) & ~(page_size - 1U);
            const auto offset = static_cast<std::size_t>(current - base);
            const auto count =
                std::min<std::size_t>(page_size - offset, end - current);
            const auto* page = find_page_locked(base);
            if (!page || !page->backing || page->shared_writable ||
                page->backing->shared_write_tracking_enabled() ||
                range_accessible_locked(
                    base, page_size, MemoryPermission::Write))
                return false;
            auto [entry, inserted] = prepared.try_emplace(base);
            if (inserted) {
                entry->second = std::make_shared<Bytes>(
                    page->instructions ? *page->instructions
                                       : page->backing->bytes);
            }
            std::copy_n(operation.data.data() + (current - operation.address),
                count, entry->second->data() + offset);
            current += count;
        }
    }
    ensure_unique_page_map_locked();
    for (auto& [base, bytes] : prepared) {
        // Use the normal executable-write notification. The HLE installer also
        // invalidates CPU code ranges, as for ordinary entry patches.
        mark_written_locked(base, page_size);
        find_page_locked(base)->instructions = std::move(bytes);
        refresh_jit_page_locked(base);
    }
    return true;
}

} // namespace ilemu
