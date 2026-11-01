// This Source Code Form is subject to the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "foundation/host_file_io.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ilemu {
HostFileIoRequest::HostFileIoRequest(
    int descriptor, std::uint64_t offset, std::vector<std::byte> bytes)
    : offset_(offset)
    , bytes_(std::move(bytes))
{
    do {
        descriptor_ = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    } while (descriptor_ < 0 && errno == EINTR);
    if (descriptor_ < 0) {
        error_ = errno;
        ready_.store(true, std::memory_order_release);
    }
}
HostFileIoRequest::~HostFileIoRequest()
{
    if (descriptor_ >= 0)
        static_cast<void>(::close(descriptor_));
}
void HostFileIoRequest::execute(bool write, bool append)
{
    if (append) {
        struct stat status { };
        if (::fstat(descriptor_, &status) != 0) {
            error_ = errno;
            ready_.store(true, std::memory_order_release);
            return;
        }
        offset_ = static_cast<std::uint64_t>(status.st_size);
    }
    ssize_t result;
    do {
        result = write ? ::pwrite(descriptor_, bytes_.data(), bytes_.size(),
                             static_cast<off_t>(offset_))
                       : ::pread(descriptor_, bytes_.data(), bytes_.size(),
                             static_cast<off_t>(offset_));
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        error_ = errno;
    else
        bytes_.resize(static_cast<std::size_t>(result));
    ready_.store(true, std::memory_order_release);
}
HostFileIo::~HostFileIo() { worker_.wait_idle(); }
std::shared_ptr<HostFileIoRequest> HostFileIo::submit(int descriptor,
    std::uint64_t offset, std::vector<std::byte> bytes, bool write, bool append)
{
    auto request = std::shared_ptr<HostFileIoRequest> { new HostFileIoRequest {
        descriptor, offset, std::move(bytes) } };
    if (!request->ready()) {
        const auto task =
            worker_.submit(HostWorkKind::Maintenance, std::nullopt,
                [request, write, append] { request->execute(write, append); });
        if (!task) {
            request->error_ = EIO;
            request->ready_.store(true, std::memory_order_release);
        }
    }
    return request;
}
} // namespace ilemu
