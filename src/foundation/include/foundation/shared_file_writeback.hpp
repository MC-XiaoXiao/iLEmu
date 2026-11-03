/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "foundation/file_page_cache.hpp"
#include "foundation/host_resource_controller.hpp"
#include <set>

namespace ilemu {

class FileWritebackRequest {
public:
    [[nodiscard]] bool ready() const
    {
        return pending_.load(std::memory_order_acquire) == 0;
    }
    [[nodiscard]] bool succeeded() const
    {
        return succeeded_.load(std::memory_order_acquire);
    }
    void wait() const;

private:
    friend class HostSharedFileWriter;
    friend class SharedFileWriteback;
    std::atomic<std::size_t> pending_ {
        1
    }; // Builder owns the final reference.
    std::atomic<bool> succeeded_ { true };
};

struct FileWritebackRange {
    std::shared_ptr<GuestFileBacking> file;
    std::uint64_t offset { };
    std::vector<std::byte> bytes;
    // Keep live vnode pages discoverable by new aliases until persistence.
    // The worker only retains ownership; it never accesses their bytes.
    std::vector<std::shared_ptr<GuestPageBacking>> pages;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<const GuestPageBytes>>>
        snapshots;
};

// Mandatory persistence uses the existing host scheduler, with one FIFO worker.
// At most 64 tasks / 16 MiB are detached; saturation drains before accepting
// another batch. This exceptional backpressure may block the producer.
class HostSharedFileWriter {
public:
    ~HostSharedFileWriter();
    [[nodiscard]] bool pending() const
    {
        return builders_.load(std::memory_order_acquire) != 0 ||
               tasks_.load(std::memory_order_acquire) != 0;
    }
    void write(FileWritebackRange range,
        std::shared_ptr<FileWritebackRequest> request);
    void synchronize(std::set<std::shared_ptr<GuestFileIoState>> files,
        std::shared_ptr<FileWritebackRequest> request);

private:
    friend class SharedFileWriteback;
    void submit(std::size_t bytes,
        std::shared_ptr<FileWritebackRequest> request,
        std::function<bool()> work);
    std::mutex producer_mutex_;
    std::atomic<std::size_t> tasks_ { };
    std::atomic<std::size_t> builders_ { };
    std::atomic<std::size_t> bytes_ { };
    HostResourceController worker_;
};

// Built only while guest execution is serialized (or while retiring a VM).
// Adjacent vnode offsets form bounded batches; workers receive immutable bytes.
class SharedFileWriteback {
public:
    explicit SharedFileWriteback(std::shared_ptr<HostSharedFileWriter> writer);
    void append(const std::shared_ptr<GuestPageBacking>& page);
    [[nodiscard]] std::shared_ptr<FileWritebackRequest> finish(
        bool synchronous);

private:
    void flush();
    std::shared_ptr<HostSharedFileWriter> writer_;
    std::shared_ptr<FileWritebackRequest> request_ {
        std::make_shared<FileWritebackRequest>()
    };
    FileWritebackRange range_;
    std::set<std::shared_ptr<GuestFileIoState>> files_;
};

} // namespace ilemu
