/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "foundation/file_page_cache.hpp"

namespace ilemu {

// Detached, initialized cache misses. Workers read only demanded missing
// ranges; the serial VM boundary validates and adopts the resulting pages.
class PreparedFilePages {
public:
    [[nodiscard]] static std::shared_ptr<PreparedFilePages> read(
        FilePageCache& cache, std::shared_ptr<GuestFileBacking> file,
        std::uint64_t offset, std::size_t size, bool shared_vnode);
    void commit(FilePageCache& cache, bool shared_vnode);

private:
    friend class FilePageCache;
    std::shared_ptr<GuestFileBacking> file_;
    GuestFileGeneration generation_;
    std::uint64_t writeback_revision_;
    std::vector<std::shared_ptr<GuestPageBacking>> pages_;
};

} // namespace ilemu
