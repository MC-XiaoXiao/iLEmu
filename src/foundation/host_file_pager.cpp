/* SPDX-License-Identifier: MPL-2.0 */
#include "foundation/host_file_pager.hpp"
#include "foundation/performance.hpp"
#include "foundation/prepared_file_pages.hpp"

#include <utility>

namespace ilemu {

void FilePageRead::read()
{
    prepared_pages_ =
        PreparedFilePages::read(*cache_, file_, offset_, size_, shared_);
    {
        const std::lock_guard lock { ready_mutex_ };
        ready_.store(true, std::memory_order_release);
    }
    ready_changed_.notify_all();
}

bool FilePageRead::wait_until(std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock { ready_mutex_ };
    return ready_changed_.wait_until(lock, deadline, [this] { return ready(); });
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

HostFilePager::HostFilePager(std::function<void()> completion_wake)
    : completion_wake_ { std::move(completion_wake) }
{
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
            read->cache_ == cache && read->offset_ < offset + size &&
            offset < read->offset_ + read->size_ && read->shared_ == shared)
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
        [this, read] {
            read->read();
            // Publish detached bytes before notifying the serial VM owner.
            // The pager drains this worker before releasing the callback.
            if (completion_wake_)
                completion_wake_();
        }));
    return read;
}

thread_local FilePageFaultScope* FilePageFaultScope::current_ {};
FilePageFaultScope::FilePageFaultScope() : previous_ { current_ }
{ current_ = this; }
FilePageFaultScope::~FilePageFaultScope() { current_ = previous_; }
void FilePageFaultScope::suspend(FilePageFault fault)
{ current_->pending_ = std::move(fault); }
}
