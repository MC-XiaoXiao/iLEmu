// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace ilemu {

// XNU keeps options on the socket retained by the shared fileglob. The
// object exists before the first set, so dup/fork also share future options.
// Per-socket locking keeps unrelated sockets independent across processes.
class SocketOptions {
public:
    using Value = std::vector<std::byte>;

    // sonewconn copies socket option bits (except SO_ACCEPTCONN), linger
    // and buffer high-water marks before the child enters the accept queue.
    // Protocol options, low-water marks and send/receive timeouts are not
    // copied by that operation. Extended so_flags need their own ABI policy.
    [[nodiscard]] std::shared_ptr<SocketOptions> accepted_snapshot() const
    {
        auto child = std::make_shared<SocketOptions>();
        std::lock_guard lock { mutex_ };
        for (const auto& [key, value] : values_) {
            if (key.first != 0xffffU)
                continue;
            switch (key.second) {
            case 0x0001: // SO_DEBUG
            case 0x0004: // SO_REUSEADDR
            case 0x0008: // SO_KEEPALIVE
            case 0x0010: // SO_DONTROUTE
            case 0x0020: // SO_BROADCAST
            case 0x0040: // SO_USELOOPBACK
            case 0x0080: // SO_LINGER (ticks)
            case 0x0100: // SO_OOBINLINE
            case 0x0200: // SO_REUSEPORT
            case 0x0400: // SO_TIMESTAMP
            case 0x0800: // SO_TIMESTAMP_MONOTONIC, when set
            case 0x2000: // SO_DONTTRUNC
            case 0x4000: // SO_WANTMORE
            case 0x8000: // SO_WANTOOBFLAG
            case 0x1001: // SO_SNDBUF
            case 0x1002: // SO_RCVBUF
            case 0x1080: // SO_LINGER_SEC
                child->values_.emplace(key, value);
                break;
            default:
                break;
            }
        }
        return child;
    }

    template <typename Apply>
    std::uint32_t update(std::uint32_t level, std::uint32_t name,
        Value value, Apply&& apply)
    {
        std::lock_guard lock { mutex_ };
        // Serialize backend changes with the guest-visible cache, and do
        // not publish a value that the backend rejected.
        if (const auto error = apply(value); error != 0)
            return error;
        values_[{ level, name }] = std::move(value);
        return 0;
    }

    [[nodiscard]] std::optional<Value> get(
        std::uint32_t level, std::uint32_t name) const
    {
        std::lock_guard lock { mutex_ };
        const auto found = values_.find({ level, name });
        if (found == values_.end())
            return std::nullopt;
        return found->second;
    }

    [[nodiscard]] bool enabled(std::uint32_t level, std::uint32_t name,
        std::size_t width = std::numeric_limits<std::size_t>::max()) const
    {
        std::lock_guard lock { mutex_ };
        const auto found = values_.find({ level, name });
        if (found == values_.end())
            return false;
        const auto& bytes = found->second;
        return std::any_of(bytes.begin(),
            bytes.begin() + static_cast<Value::difference_type>(
                                std::min(bytes.size(), width)),
            [](std::byte byte) { return byte != std::byte { 0 }; });
    }

private:
    mutable std::mutex mutex_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Value> values_;
};

} // namespace ilemu
