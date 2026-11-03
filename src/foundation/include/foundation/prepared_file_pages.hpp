/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "foundation/file_page_cache.hpp"

namespace ilemu {

// Detached, initialized physical pages. Workers own these exclusively until
// the serial VM boundary validates the snapshot and adopts cache misses.
class PreparedFilePages {
public:
    PreparedFilePages(std::shared_ptr<GuestFileBacking> file,
        std::uint64_t offset, std::span<const std::byte> bytes,
        GuestFileGeneration generation, std::uint64_t writeback_revision);
    void commit(FilePageCache& cache, bool shared_vnode);

private:
    friend class FilePageCache;
    std::shared_ptr<GuestFileBacking> file_;
    GuestFileGeneration generation_;
    std::uint64_t writeback_revision_;
    std::vector<std::shared_ptr<GuestPageBacking>> pages_;
};

} // namespace ilemu
