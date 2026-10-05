// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include "foundation/file_page_cache.hpp"

#include <algorithm>

namespace ilemu {

std::shared_ptr<GuestPageBacking> FilePageCache::intern_immutable_page(
    std::span<const std::byte> bytes)
{
    if (bytes.size() != guest_memory_page_size)
        return {};
    const Key key { {}, {}, 0, sha256(bytes), 0, guest_memory_page_size,
        true, false, true };
    const std::scoped_lock lock { mutex_ };
    if (auto cached = find_page_locked(key)) {
        if (std::equal(bytes.begin(), bytes.end(), cached->bytes.begin()))
            return cached;
        // A digest collision must never substitute different guest bytes.
        auto uncached = std::make_shared<GuestPageBacking>();
        std::copy(bytes.begin(), bytes.end(), uncached->bytes.begin());
        return uncached;
    }
    auto page = std::make_shared<GuestPageBacking>();
    std::copy(bytes.begin(), bytes.end(), page->bytes.begin());
    lru_.push_back(key);
    pages_.emplace(key, PageRecord { page, std::prev(lru_.end()) });
    evict_locked();
    return page;
}

bool AddressSpace::copy_in_immutable_page(std::uint32_t address,
    std::span<const std::byte> data)
{
    if (address % page_size != 0 || data.size() != page_size)
        return false;
    auto lock = write_lock();
    if (!range_accessible_locked(address, page_size, MemoryPermission::None))
        return false;
    ensure_unique_page_map_locked();
    auto& page = ensure_page_locked(address);
    if (page.shared_writable || page.file_writeback_capable) {
        const CopyInOperation operation { address, data };
        return copy_in_batch_locked(std::span { &operation, 1U }, false);
    }
    auto backing = file_page_cache_->intern_immutable_page(data);
    if (!backing)
        return false;
    if (!page.backing)
        record_resident_page_locked();
    page.backing = std::move(backing);
    page.file_cached = true;
    page.copy_on_write_possible = true;
    refresh_jit_page_locked(address);
    mark_written_locked(address, page_size);
    return true;
}

} // namespace ilemu
