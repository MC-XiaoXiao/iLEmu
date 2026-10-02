// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
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
