/* SPDX-License-Identifier: MPL-2.0 */
#include "foundation/shared_file_writeback.hpp"
#include <algorithm>
#include <cerrno>
#include <unistd.h>

namespace ilemu {
namespace {
    constexpr std::size_t maximum_batch_bytes = 256U * 1024U;

    bool write_range(const FileWritebackRange& range)
    {
        const auto& file = range.file;
        const auto descriptor = file->io_state->file_descriptor;
        std::size_t written = 0;
        while (written < range.bytes.size()) {
            const auto result = ::pwrite(descriptor,
                range.bytes.data() + written, range.bytes.size() - written,
                static_cast<off_t>(range.offset + written));
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0)
                break;
            written += static_cast<std::size_t>(result);
        }
        if (written && file->object_identity)
            file->object_identity->writeback_revision.fetch_add(
                1, std::memory_order_release);
        if (written) {
            if (auto registry = file->generation_registry.lock())
                static_cast<void>(registry->publish_descriptor(file->path,
                    descriptor, GuestFileMutationKind::SharedWriteback));
        }
        {
            const auto& owner = file->object_identity;
            const std::scoped_lock lock { owner->writeback_mutex };
            for (const auto& [offset, snapshot] : range.snapshots) {
                const auto entry = owner->pending_writeback_pages.find(offset);
                if (entry != owner->pending_writeback_pages.end() &&
                    entry->second == snapshot)
                    owner->pending_writeback_pages.erase(entry);
            }
        }
        return written == range.bytes.size();
    }
}

HostSharedFileWriter::~HostSharedFileWriter() { worker_.wait_idle(); }

void FileWritebackRequest::wait() const
{
    auto pending = pending_.load(std::memory_order_acquire);
    while (pending != 0) {
        pending_.wait(pending, std::memory_order_acquire);
        pending = pending_.load(std::memory_order_acquire);
    }
}

void HostSharedFileWriter::submit(std::size_t bytes,
    std::shared_ptr<FileWritebackRequest> request, std::function<bool()> work)
{
    std::lock_guard producer { producer_mutex_ };
    if (tasks_.load(std::memory_order_acquire) >= 64U ||
        bytes > 16U * 1024U * 1024U - bytes_.load(std::memory_order_acquire))
        worker_.wait_idle();
    tasks_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(bytes, std::memory_order_relaxed);
    request->pending_.fetch_add(1, std::memory_order_relaxed);
    static_cast<void>(worker_.submit(HostWorkKind::Maintenance, std::nullopt,
        [this, bytes, request = std::move(request),
            work = std::move(work)]() mutable {
            if (!work())
                request->succeeded_.store(false, std::memory_order_release);
            work = { };
            bytes_.fetch_sub(bytes, std::memory_order_release);
            request->pending_.fetch_sub(1, std::memory_order_release);
            request->pending_.notify_all();
            tasks_.fetch_sub(1, std::memory_order_release);
        }));
}

void HostSharedFileWriter::write(
    FileWritebackRange range, std::shared_ptr<FileWritebackRequest> request)
{
    const auto size = range.bytes.size();
    submit(size, std::move(request),
        [range = std::move(range)] { return write_range(range); });
}

void HostSharedFileWriter::synchronize(
    std::set<std::shared_ptr<GuestFileIoState>> files,
    std::shared_ptr<FileWritebackRequest> request)
{
    submit(0, std::move(request), [files = std::move(files)] {
        bool succeeded = true;
        for (const auto& file : files)
            if (!file->synchronize())
                succeeded = false;
        return succeeded;
    });
}

SharedFileWriteback::SharedFileWriteback(
    std::shared_ptr<HostSharedFileWriter> writer)
    : writer_ { std::move(writer) }
{
    if (writer_)
        writer_->builders_.fetch_add(1, std::memory_order_acq_rel);
}

void SharedFileWriteback::append(const std::shared_ptr<GuestPageBacking>& page)
{
    const auto& file = page->file_writeback_;
    files_.insert(file->io_state);
    const std::scoped_lock lock { page->mutex_ };
    const auto count = page->file_byte_count_;
    if (page->file_flushed_bytes_ &&
        (page->file_writeback_request_ == request_ ||
            (page->file_writeback_request_->ready() &&
                page->file_writeback_request_->succeeded())) &&
        std::equal(page->bytes.begin(), page->bytes.begin() + count,
            page->file_flushed_bytes_->begin()))
        return;
    if (range_.file &&
        (range_.file->io_state != file->io_state ||
            range_.offset + range_.bytes.size() != page->file_offset_ ||
            range_.bytes.size() + count > maximum_batch_bytes))
        flush();
    if (!range_.file) {
        range_.file = file;
        range_.offset = page->file_offset_;
    }
    range_.bytes.insert(
        range_.bytes.end(), page->bytes.begin(), page->bytes.begin() + count);
    range_.pages.push_back(page);
    page->file_flushed_bytes_ =
        std::make_shared<const GuestPageBytes>(page->bytes);
    page->file_writeback_request_ = request_;
    range_.snapshots.emplace_back(
        page->file_offset_, page->file_flushed_bytes_);
    {
        const auto& owner = file->object_identity;
        const std::scoped_lock vnode_lock { owner->writeback_mutex };
        owner->pending_writeback_pages[page->file_offset_] =
            page->file_flushed_bytes_;
    }
}

void SharedFileWriteback::flush()
{
    if (!range_.file)
        return;
    if (writer_)
        writer_->write(std::move(range_), request_);
    else if (!write_range(range_))
        request_->succeeded_.store(false, std::memory_order_release);
    range_ = { };
}

std::shared_ptr<FileWritebackRequest> SharedFileWriteback::finish(
    bool synchronous)
{
    flush();
    if (synchronous && !files_.empty()) {
        if (writer_)
            writer_->synchronize(std::move(files_), request_);
        else
            for (const auto& file : files_)
                if (!file->synchronize())
                    request_->succeeded_.store(
                        false, std::memory_order_release);
    }
    request_->pending_.fetch_sub(1, std::memory_order_release);
    request_->pending_.notify_all();
    if (writer_)
        writer_->builders_.fetch_sub(1, std::memory_order_release);
    return request_;
}
} // namespace ilemu
