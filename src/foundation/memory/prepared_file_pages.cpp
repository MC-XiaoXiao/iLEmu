/* SPDX-License-Identifier: MPL-2.0 */
#include "foundation/prepared_file_pages.hpp"
#include "foundation/performance.hpp"

#include <algorithm>
#include <utility>

namespace ilemu {

PreparedFilePages::PreparedFilePages(std::shared_ptr<GuestFileBacking> file,
    std::uint64_t offset, std::span<const std::byte> bytes,
    GuestFileGeneration generation, std::uint64_t writeback_revision)
    : file_ { std::move(file) }
    , generation_ { generation }
    , writeback_revision_ { writeback_revision }
{
    pages_.reserve((bytes.size() + guest_memory_page_size - 1U) /
        guest_memory_page_size);
    for (std::size_t start = 0; start < bytes.size(); start += guest_memory_page_size) {
        auto page = std::make_shared<GuestPageBacking>();
        const auto count = static_cast<std::uint32_t>(
            std::min<std::size_t>(guest_memory_page_size, bytes.size() - start));
        std::copy_n(bytes.data() + start, count, page->bytes.begin());
        page->file_writeback_ = file_;
        page->file_offset_ = offset + start;
        page->file_byte_count_ = count;
        page->has_file_source_ = true;
        pages_.push_back(std::move(page));
    }
}

void PreparedFilePages::commit(FilePageCache& cache, bool shared_vnode)
{
    const auto& vnode = file_->object_identity;
    if (file_descriptor_generation(file_->io_state->file_descriptor) != generation_ ||
        vnode->writeback_revision.load(std::memory_order_acquire) != writeback_revision_)
        return;
    const PerformanceLatencyScope publish { PerfLatencyKind::VmCachePublish };
    cache.publish_pages(*this, shared_vnode);
}

} // namespace ilemu
