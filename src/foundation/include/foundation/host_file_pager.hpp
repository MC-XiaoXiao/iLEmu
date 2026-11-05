/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "foundation/file_page_cache.hpp"
#include "foundation/host_resource_controller.hpp"
#include "foundation/file_page_fault.hpp"

#include <functional>

namespace ilemu {

// Owns only retained vnode input and detached host bytes. Guest publication is
// a separate operation at the serial VM boundary.
class FilePageRead {
public:
    [[nodiscard]] bool ready() const noexcept
    { return ready_.load(std::memory_order_acquire); }
    void commit();

private:
    friend class HostFilePager;
    void read();
    std::shared_ptr<FilePageCache> cache_;
    std::shared_ptr<GuestFileBacking> file_;
    std::uint64_t offset_ { };
    std::size_t size_ { };
    std::shared_ptr<PreparedFilePages> prepared_pages_;
    bool shared_ { };
    bool consumed_ { };
    std::atomic<bool> ready_ { false };
};

class HostFilePager {
public:
    explicit HostFilePager(std::function<void()> completion_wake = {});
    ~HostFilePager();
    [[nodiscard]] std::shared_ptr<FilePageRead> prepare(
        std::shared_ptr<FilePageCache> cache,
        std::shared_ptr<GuestFileBacking> file,
        std::uint64_t offset, std::size_t size, bool shared);

private:
    // Includes queued, running and completed input. At capacity a fault waits
    // for the oldest ticket and retries; it never falls back to blocking I/O.
    static constexpr std::size_t maximum_reads = 64;
    std::mutex mutex_;
    std::deque<std::shared_ptr<FilePageRead>> reads_;
    std::function<void()> completion_wake_;
    HostResourceController worker_;
};

}
