/* SPDX-License-Identifier: MPL-2.0 */
#include "foundation/host_file_pager.hpp"
#include "foundation/performance.hpp"

#include <algorithm>
#include <cerrno>
#include <utility>
#include <unistd.h>

namespace ilemu {

void FilePageRead::read()
{
    const auto descriptor = file_->io_state->file_descriptor;
    const auto before = file_descriptor_generation(descriptor);
    writeback_revision_ = file_->object_identity->writeback_revision.load(
        std::memory_order_acquire);
    std::size_t received = 0;
    while (received < bytes_.size()) {
        const auto count = ::pread(descriptor, bytes_.data() + received,
            bytes_.size() - received, static_cast<off_t>(offset_ + received));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        received += static_cast<std::size_t>(count);
    }
    const auto after = file_descriptor_generation(descriptor);
    if (before && before == after)
        generation_ = after;
    ready_.store(true, std::memory_order_release);
}

void FilePageRead::commit()
{
    const PerformanceLatencyScope timing { PerfLatencyKind::VmPageCommit };
    if (consumed_)
        return;
    consumed_ = true;
    const auto& vnode = file_->object_identity;
    if (generation_ &&
        generation_ == file_descriptor_generation(file_->io_state->file_descriptor) &&
        writeback_revision_ == vnode->writeback_revision.load(std::memory_order_acquire)) {
        const PerformanceLatencyScope publish { PerfLatencyKind::VmCachePublish };
        for (std::size_t offset = 0; offset < bytes_.size();
             offset += guest_memory_page_size) {
            const auto count = static_cast<std::uint32_t>(
                std::min<std::size_t>(guest_memory_page_size, bytes_.size() - offset));
            auto page = cache_->load_page(file_, offset_ + offset, count, shared_);
            const std::lock_guard page_lock { page->mutex_ };
            const std::lock_guard pending_lock { vnode->writeback_mutex };
            // Resident aliases and descriptor reflection always win.
            if (!page->file_backing_)
                continue;
            std::copy_n(bytes_.data() + offset, count, page->bytes.begin());
            std::fill(page->bytes.begin() + count, page->bytes.end(), std::byte {});
            if (const auto pending = vnode->pending_writeback_pages.find(offset_ + offset);
                pending != vnode->pending_writeback_pages.end())
                std::copy_n(pending->second->begin(), count, page->bytes.begin());
            page->file_backing_.reset();
        }
    }
    std::vector<std::byte> {}.swap(bytes_);
}

HostFilePager::~HostFilePager() { worker_.wait_idle(); }

std::shared_ptr<FilePageRead> HostFilePager::prepare(
    std::shared_ptr<FilePageCache> cache, std::shared_ptr<GuestFileBacking> file,
    std::uint64_t offset, std::size_t size, bool shared)
{
    const std::lock_guard lock { mutex_ };
    std::erase_if(reads_, [](const auto& read) { return read->consumed_; });
    for (const auto& read : reads_)
        if (read->file_->object_identity == file->object_identity &&
            read->cache_ == cache && read->offset_ == offset &&
            read->bytes_.size() == size && read->shared_ == shared)
            return read;
    if (reads_.size() == maximum_reads)
        return reads_.front();
    auto read = std::make_shared<FilePageRead>();
    read->cache_ = std::move(cache);
    read->file_ = std::move(file);
    read->offset_ = offset;
    read->bytes_.resize(size);
    read->shared_ = shared;
    reads_.push_back(read);
    static_cast<void>(worker_.submit(HostWorkKind::Maintenance, std::nullopt,
        [read] { read->read(); }));
    return read;
}

thread_local FilePageFaultScope* FilePageFaultScope::current_ {};
FilePageFaultScope::FilePageFaultScope() : previous_ { current_ }
{ current_ = this; }
FilePageFaultScope::~FilePageFaultScope() { current_ = previous_; }
void FilePageFaultScope::suspend(FilePageFault fault)
{ current_->pending_ = std::move(fault); }
}
