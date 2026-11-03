/* SPDX-License-Identifier: MPL-2.0 */
#include "foundation/host_file_pager.hpp"
#include "foundation/performance.hpp"
#include "foundation/prepared_file_pages.hpp"

#include <cerrno>
#include <utility>
#include <unistd.h>

namespace ilemu {

void FilePageRead::read()
{
    const auto descriptor = file_->io_state->file_descriptor;
    const auto before = file_descriptor_generation(descriptor);
    const auto revision = file_->object_identity->writeback_revision.load(
        std::memory_order_acquire);
    std::vector<std::byte> bytes(size_);
    std::size_t received = 0;
    while (received < bytes.size()) {
        const auto count = ::pread(descriptor, bytes.data() + received,
            bytes.size() - received, static_cast<off_t>(offset_ + received));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        received += static_cast<std::size_t>(count);
    }
    const auto after = file_descriptor_generation(descriptor);
    if (before && before == after)
        prepared_pages_ = std::make_shared<PreparedFilePages>(
            file_, offset_, bytes, *after, revision);
    ready_.store(true, std::memory_order_release);
}

void FilePageRead::commit()
{
    const PerformanceLatencyScope timing { PerfLatencyKind::VmPageCommit };
    if (consumed_)
        return;
    consumed_ = true;
    if (prepared_pages_) {
        prepared_pages_->commit(*cache_, shared_);
        prepared_pages_.reset();
    }
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
            read->size_ == size && read->shared_ == shared)
            return read;
    if (reads_.size() == maximum_reads)
        return reads_.front();
    auto read = std::make_shared<FilePageRead>();
    read->cache_ = std::move(cache);
    read->file_ = std::move(file);
    read->offset_ = offset;
    read->size_ = size;
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
