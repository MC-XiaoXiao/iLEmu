/* SPDX-License-Identifier: MPL-2.0 */
#include "foundation/prepared_file_pages.hpp"
#include "foundation/performance.hpp"

#include <algorithm>
#include <cerrno>
#include <unistd.h>
#include <utility>

namespace ilemu {

std::shared_ptr<PreparedFilePages> PreparedFilePages::read(FilePageCache& cache,
    std::shared_ptr<GuestFileBacking> file, std::uint64_t offset,
    std::size_t size, bool shared_vnode)
{
    const auto descriptor = file->io_state->file_descriptor;
    const auto before = file_descriptor_generation(descriptor);
    if (!before)
        return { };
    auto prepared = std::make_shared<PreparedFilePages>();
    prepared->file_ = std::move(file);
    prepared->generation_ = *before;
    prepared->writeback_revision_ =
        prepared->file_->object_identity->writeback_revision.load(
            std::memory_order_acquire);
    const auto end = offset + size;
    const auto missing = [&](std::uint64_t start) {
        return cache.needs_file_read(prepared->file_, start,
            static_cast<std::uint32_t>(
                std::min<std::uint64_t>(guest_memory_page_size, end - start)),
            shared_vnode);
    };
    while (offset < end) {
        if (!missing(offset)) {
            offset += guest_memory_page_size;
            continue;
        }
        const auto start = offset;
        do {
            offset =
                std::min<std::uint64_t>(end, offset + guest_memory_page_size);
        } while (offset < end && missing(offset));
        std::vector<std::byte> bytes(offset - start);
        std::size_t received = 0;
        while (received < bytes.size()) {
            const auto count = ::pread(descriptor, bytes.data() + received,
                bytes.size() - received, static_cast<off_t>(start + received));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            received += static_cast<std::size_t>(count);
        }
        for (std::size_t pos = 0; pos < bytes.size();
            pos += guest_memory_page_size) {
            auto page = std::make_shared<GuestPageBacking>();
            const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(
                guest_memory_page_size, bytes.size() - pos));
            std::copy_n(bytes.data() + pos, count, page->bytes.begin());
            page->file_writeback_ = prepared->file_;
            page->file_offset_ = start + pos;
            page->file_byte_count_ = count;
            page->has_file_source_ = true;
            prepared->pages_.push_back(std::move(page));
        }
    }
    if (prepared->pages_.empty() ||
        file_descriptor_generation(descriptor) != before)
        return { };
    return prepared;
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
