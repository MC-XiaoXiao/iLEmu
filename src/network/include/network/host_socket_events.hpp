// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace ilemu {

// Host-only readiness vocabulary. No host fd or poll ABI escapes the backend.
enum HostSocketEvent : std::uint8_t {
    SocketReadable = 1,
    SocketWritable = 2,
    SocketDisconnected = 4,
    SocketError = 8,
    SocketReadClosed = 16,
};

class HostSocketEvents {
public:
    class Subscription {
    public:
        ~Subscription();
        [[nodiscard]] bool sampled() const;
    private:
        friend class HostSocketEvents;
        Subscription(std::uint64_t resource, std::uint64_t identifier);
        std::uint64_t resource_, identifier_;
    };

    static HostSocketEvents& instance();
    std::uint64_t attach(int descriptor);
    void detach(std::uint64_t resource);
    void invalidate(std::uint64_t resource);
    [[nodiscard]] std::shared_ptr<Subscription> subscribe(
        std::uint64_t resource, std::uint8_t events);
    [[nodiscard]] std::uint8_t readiness(std::uint64_t resource) const;

    // Called only at the serial kernel boundary. The worker publishes versioned
    // notifications; this method commits their host readiness snapshots.
    void commit();
    [[nodiscard]] std::uint64_t generation() const;
    void set_wake(std::function<void()> wake);

private:
    HostSocketEvents();
    ~HostSocketEvents();
    void unsubscribe(std::uint64_t resource, std::uint64_t identifier);
    [[nodiscard]] bool sampled(std::uint64_t resource,
        std::uint64_t identifier) const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ilemu
