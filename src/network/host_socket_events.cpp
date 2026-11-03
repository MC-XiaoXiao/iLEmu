// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "network/host_socket_events.hpp"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ilemu {

struct HostSocketEvents::Impl {
    struct Watch {
        std::uint8_t events;
        std::uint64_t sampled_version {}, committed_version {};
    };
    struct Resource {
        int descriptor;
        std::uint64_t version { 1 };
        std::map<std::uint64_t, Watch> watches;
        std::uint8_t observed {}, committed {};
        std::uint8_t notification_mask {}, notification_bits {};
    };
    struct Sample {
        std::uint64_t resource, version;
        std::uint8_t events;
        std::vector<std::uint64_t> snapshots;
    };
    mutable std::mutex mutex;
    std::map<std::uint64_t, Resource> resources;
    std::uint64_t next_resource { 1 }, next_watch { 1 };
    std::atomic_uint64_t generation { 1 }, committed_generation { 1 };
    int wake_pipe[2] { -1, -1 };
    bool stopping {};
    std::thread worker;
    std::mutex wake_mutex;
    std::function<void()> wake;

    void interrupt()
    {
        const char byte = 0;
        while (::write(wake_pipe[1], &byte, 1) < 0 && errno == EINTR) { }
    }

    void run()
    {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "ilemu-sockets");
#elif defined(__APPLE__)
        pthread_setname_np("ilemu-sockets");
#endif
        for (;;) {
            std::vector<pollfd> descriptors { { wake_pipe[0], POLLIN, 0 } };
            std::vector<Sample> samples;
            bool snapshot = false;
            {
                std::lock_guard lock { mutex };
                if (stopping)
                    return;
                for (auto& [identifier, resource] : resources) {
                    Sample sample { identifier, resource.version, 0, {} };
                    std::uint8_t requested = 0;
                    for (const auto& [watch_id, watch] : resource.watches) {
                        requested |= watch.events;
                        if (watch.sampled_version != resource.version) {
                            sample.snapshots.push_back(watch_id);
                            sample.events |= watch.events;
                        }
                    }
                    snapshot |= !sample.snapshots.empty();
                    // A positive level is retained until an actual socket
                    // operation invalidates it. Never continuously poll OUT,
                    // EOF or an unread packet that the guest has not consumed.
                    if ((resource.observed & (SocketDisconnected | SocketError)) == 0)
                        sample.events |= requested & ~resource.observed;
                    if (sample.events == 0 && sample.snapshots.empty())
                        continue;
                    short events = 0;
                    if (sample.events & SocketReadable) {
                        events |= POLLIN;
#ifdef POLLRDHUP
                        events |= POLLRDHUP;
#endif
                    }
                    if (sample.events & SocketWritable)
                        events |= POLLOUT;
                    descriptors.push_back({ resource.descriptor, events, 0 });
                    samples.push_back(std::move(sample));
                }
            }
            const auto count = ::poll(descriptors.data(), descriptors.size(), snapshot ? 0 : -1);
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error { "host socket event wait failed" };
            }
            if (descriptors.front().revents) {
                char bytes[256];
                while (::read(wake_pipe[0], bytes, sizeof(bytes)) > 0) { }
            }
            bool published = false;
            {
                std::lock_guard lock { mutex };
                for (std::size_t i = 0; i < samples.size(); ++i) {
                    const auto& sample = samples[i];
                    const auto found = resources.find(sample.resource);
                    if (found == resources.end() || found->second.version != sample.version)
                        continue;
                    const auto revents = descriptors[i + 1].revents;
                    if (!revents && sample.snapshots.empty())
                        continue;
                    auto& resource = found->second;
                    std::uint8_t ready = 0;
                    if (revents & POLLIN) ready |= SocketReadable;
                    if (revents & POLLOUT) ready |= SocketWritable;
#ifdef POLLRDHUP
                    if (revents & POLLRDHUP) ready |= SocketReadable | SocketReadClosed;
#endif
                    if (revents & POLLHUP) ready |= SocketDisconnected;
                    if (revents & (POLLERR | POLLNVAL)) ready |= SocketError;
                    const auto mask = static_cast<std::uint8_t>(
                        sample.events | SocketDisconnected | SocketError |
                        ((sample.events & SocketReadable) ? SocketReadClosed : 0));
                    resource.observed = (resource.observed & ~mask) | ready;
                    resource.notification_bits = (resource.notification_bits & ~mask) | ready;
                    resource.notification_mask |= mask;
                    for (const auto watch_id : sample.snapshots) {
                        if (const auto watch = resource.watches.find(watch_id);
                            watch != resource.watches.end())
                            watch->second.sampled_version = sample.version;
                    }
                    published = true;
                }
                if (published)
                    generation.fetch_add(1, std::memory_order_release);
            }
            if (published) {
                // This callback contains host wake primitives only. Unregister
                // serializes with an in-flight invocation during session exit.
                std::lock_guard lock { wake_mutex };
                if (wake)
                    wake();
            }
        }
    }
};

HostSocketEvents& HostSocketEvents::instance()
{
    static HostSocketEvents events;
    return events;
}

HostSocketEvents::HostSocketEvents() : impl_ { std::make_unique<Impl>() } { }

HostSocketEvents::~HostSocketEvents()
{
    if (!impl_->worker.joinable())
        return;
    {
        std::lock_guard lock { impl_->mutex };
        impl_->stopping = true;
        impl_->interrupt();
    }
    impl_->worker.join();
    ::close(impl_->wake_pipe[0]);
    ::close(impl_->wake_pipe[1]);
}

std::uint64_t HostSocketEvents::attach(int descriptor)
{
    std::lock_guard lock { impl_->mutex };
    if (!impl_->worker.joinable()) {
        if (::pipe(impl_->wake_pipe) != 0)
            throw std::runtime_error { "cannot create socket event wake pipe" };
        for (const auto fd : impl_->wake_pipe) {
            if (::fcntl(fd, F_SETFL, O_NONBLOCK) < 0 ||
                ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
                throw std::runtime_error { "cannot configure socket event wake pipe" };
        }
        impl_->worker = std::thread { [this] { impl_->run(); } };
    }
    const auto identifier = impl_->next_resource++;
    Impl::Resource resource;
    resource.descriptor = descriptor;
    impl_->resources.emplace(identifier, std::move(resource));
    return identifier;
}

void HostSocketEvents::detach(std::uint64_t resource)
{
    std::lock_guard lock { impl_->mutex };
    impl_->resources.erase(resource);
    impl_->interrupt();
}

void HostSocketEvents::invalidate(std::uint64_t identifier)
{
    std::lock_guard lock { impl_->mutex };
    auto& resource = impl_->resources.at(identifier);
    ++resource.version;
    resource.observed = resource.committed = 0;
    resource.notification_mask = resource.notification_bits = 0;
    if (!resource.watches.empty())
        impl_->interrupt();
}

std::shared_ptr<HostSocketEvents::Subscription> HostSocketEvents::subscribe(
    std::uint64_t identifier, std::uint8_t events)
{
    std::lock_guard lock { impl_->mutex };
    const auto watch = impl_->next_watch++;
    impl_->resources.at(identifier).watches.emplace(watch, Impl::Watch { static_cast<std::uint8_t>(events | SocketDisconnected | SocketError) });
    impl_->interrupt();
    return std::shared_ptr<Subscription> { new Subscription { identifier, watch } };
}

std::uint8_t HostSocketEvents::readiness(std::uint64_t identifier) const
{
    std::lock_guard lock { impl_->mutex };
    return impl_->resources.at(identifier).committed;
}

void HostSocketEvents::commit()
{
    if (impl_->committed_generation.load(std::memory_order_acquire) ==
        impl_->generation.load(std::memory_order_acquire))
        return;
    std::lock_guard lock { impl_->mutex };
    for (auto& [identifier, resource] : impl_->resources) {
        if (!resource.notification_mask)
            continue;
        resource.committed = (resource.committed & ~resource.notification_mask) |
                             resource.notification_bits;
        resource.notification_mask = resource.notification_bits = 0;
        for (auto& [watch_id, watch] : resource.watches)
            watch.committed_version = watch.sampled_version;
    }
    impl_->committed_generation.store(impl_->generation.load(std::memory_order_relaxed),
        std::memory_order_release);
}

std::uint64_t HostSocketEvents::generation() const
{
    return impl_->generation.load(std::memory_order_acquire);
}

void HostSocketEvents::set_wake(std::function<void()> wake)
{
    std::lock_guard lock { impl_->wake_mutex };
    impl_->wake = std::move(wake);
}

HostSocketEvents::Subscription::Subscription(std::uint64_t resource, std::uint64_t identifier)
    : resource_ { resource }, identifier_ { identifier } { }

HostSocketEvents::Subscription::~Subscription()
{
    HostSocketEvents::instance().unsubscribe(resource_, identifier_);
}

bool HostSocketEvents::Subscription::sampled() const
{
    return HostSocketEvents::instance().sampled(resource_, identifier_);
}

void HostSocketEvents::unsubscribe(std::uint64_t resource, std::uint64_t identifier)
{
    std::lock_guard lock { impl_->mutex };
    if (const auto found = impl_->resources.find(resource); found != impl_->resources.end())
        found->second.watches.erase(identifier);
    impl_->interrupt();
}

bool HostSocketEvents::sampled(std::uint64_t identifier, std::uint64_t watch_id) const
{
    std::lock_guard lock { impl_->mutex };
    const auto found = impl_->resources.find(identifier);
    if (found == impl_->resources.end())
        return true;
    const auto& resource = found->second;
    return resource.watches.at(watch_id).committed_version == resource.version;
}

} // namespace ilemu
