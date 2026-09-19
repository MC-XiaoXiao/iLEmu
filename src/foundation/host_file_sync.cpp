// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/host_file_sync.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace ilemu {

HostFileSyncRequest::HostFileSyncRequest(int descriptor)
{
    do {
        descriptor_ = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    } while (descriptor_ < 0 && errno == EINTR);
    if (descriptor_ < 0)
        error_.store(errno, std::memory_order_relaxed);
}

HostFileSyncRequest::~HostFileSyncRequest()
{
    if (descriptor_ >= 0)
        static_cast<void>(::close(descriptor_));
}

std::optional<int> HostFileSyncRequest::result() const
{
    const auto error = error_.load(std::memory_order_acquire);
    return error < 0 ? std::nullopt : std::optional<int> { error };
}

HostFileSynchronizer::~HostFileSynchronizer()
{
    // Process exit may abandon a guest waiter, but must not discard a sync
    // already issued to the host. No worker accesses guest state.
    worker_.wait_idle();
}

std::shared_ptr<HostFileSyncRequest> HostFileSynchronizer::synchronize(
    int descriptor)
{
    auto request = std::shared_ptr<HostFileSyncRequest> {
        new HostFileSyncRequest { descriptor }
    };
    if (request->result())
        return request;
    const auto task = worker_.submit(HostWorkKind::Maintenance, std::nullopt,
        [request] {
            int result;
            do {
                result = ::fsync(request->descriptor_);
            } while (result != 0 && errno == EINTR);
            request->error_.store(result == 0 ? 0 : errno,
                std::memory_order_release);
        });
    if (!task)
        request->error_.store(EIO, std::memory_order_release);
    return request;
}

} // namespace ilemu
