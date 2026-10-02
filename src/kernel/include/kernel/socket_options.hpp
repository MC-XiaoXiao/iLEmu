// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "device_state/darwin_abi.hpp"

#include <algorithm>
#include <atomic>
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

    // sooptcopyin rejects short fixed values before touching user memory
    // and ignores bytes beyond the native scalar or linger structure.
    [[nodiscard]] static std::optional<std::size_t> input_size(
        std::uint32_t level, std::uint32_t name)
    {
        if (level != socket_level)
            return std::nullopt;
        if (is_linger(name))
            return 8;
        if (is_option_bit(name) || name == defunct_option)
            return 4;
        return std::nullopt;
    }

    // sonewconn copies socket option bits (except SO_ACCEPTCONN), linger
    // and buffer high-water marks before the child enters the accept queue.
    // Protocol options, low-water marks and send/receive timeouts are not
    // copied by that operation. Extended so_flags need their own ABI policy.
    [[nodiscard]] std::shared_ptr<SocketOptions> accepted_snapshot() const
    {
        auto child = std::make_shared<SocketOptions>();
        std::lock_guard lock { mutex_ };
        child->linger_ = linger_;
        for (const auto& [key, value] : values_) {
            if (key.first == socket_level &&
                (is_option_bit(key.second) || key.second == 0x1001 ||
                    key.second == 0x1002)) // buffer high-water marks
                child->values_.emplace(key, value);
        }
        return child;
    }

    template <typename Apply>
    std::uint32_t update(std::uint32_t level, std::uint32_t name,
        Value value, Apply&& apply)
    {
        std::lock_guard lock { mutex_ };
        // sosetopt checks SOF_DEFUNCT after fixed copyin, before privilege
        // checks. A retained socket cannot opt back in or out once defunct.
        if (level == socket_level && name == defunct_option && defunct())
            return 9; // EBADF
        // Serialize backend changes with the guest-visible cache, and do
        // not publish a value that the backend rejected.
        if (const auto error = apply(value); error != 0)
            return error;
        if (level == socket_level) {
            if (is_linger(name)) {
                linger_.enabled = read_word(value, 0) != 0;
                // Native so_linger is a signed short. Unsigned arithmetic
                // preserves narrowing even when the seconds multiply wraps.
                const auto ticks = read_word(value, 4) *
                    (name == linger_seconds ? ticks_per_second : 1U);
                linger_.ticks = static_cast<std::int32_t>(ticks & 0xffffU);
                if (linger_.ticks >= 0x8000)
                    linger_.ticks -= 0x10000;
                return 0;
            }
            if (is_option_bit(name))
                write_word(value, 0, read_word(value, 0) != 0 ? name : 0U);
            else if (name == defunct_option)
                write_word(value, 0, read_word(value, 0) != 0 ? 1U : 0U);
        }
        values_[{ level, name }] = std::move(value);
        return 0;
    }

    // Eligibility and the terminal flag belong to the shared socket, not an
    // fd. Claim the transition once, without allocating an option copy. The
    // capture callback may use the host transport, but must not acquire guest
    // connection-queue locks used by accepted_snapshot(). Guest queue teardown
    // follows after this lock is released.
    template <typename CaptureError>
    [[nodiscard]] bool begin_defunct(bool default_eligible, CaptureError&& capture_error)
    {
        std::lock_guard lock { mutex_ };
        if (defunct())
            return false;
        const auto option = values_.find({ socket_level, defunct_option });
        const auto eligible = option == values_.end()
            ? default_eligible : read_word(option->second, 0) != 0;
        if (!eligible)
            return false;
        const auto error = capture_error();
        terminal_state_.store(defunct_bit | (error != 0 ? error : 9U),
            std::memory_order_relaxed); // sodefunct preserves an existing so_error
        return true;
    }

    // Publish the monotonic flag and pending error in one atomic word. Hot
    // I/O checks remain a relaxed load; aliases consume the error only once.
    [[nodiscard]] bool defunct() const noexcept
    {
        return (terminal_state_.load(std::memory_order_relaxed) & defunct_bit) != 0;
    }

    [[nodiscard]] std::uint32_t take_defunct_error() noexcept
    {
        return terminal_state_.fetch_and(defunct_bit, std::memory_order_relaxed) &
            ~defunct_bit;
    }

    // XNU accept checks an empty nonblocking queue before so_error, and
    // consumes that error before removing any completed connection.
    [[nodiscard]] std::optional<std::uint32_t> defunct_accept_error(
        bool nonblocking, bool queued) noexcept
    {
        if (!defunct())
            return std::nullopt;
        if (nonblocking && !queued)
            return 35; // EWOULDBLOCK
        if (const auto error = take_defunct_error())
            return error;
        if (!queued)
            return 53; // ECONNABORTED
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Value> get(
        std::uint32_t level, std::uint32_t name,
        DarwinSocketLingerAbi linger_abi = DarwinSocketLingerAbi::OptionMask) const
    {
        std::lock_guard lock { mutex_ };
        if (level == socket_level && is_linger(name)) {
            Value value(8);
            const auto enabled = linger_abi == DarwinSocketLingerAbi::OptionMask
                ? linger_ticks : 1U;
            write_word(value, 0, linger_.enabled ? enabled : 0U);
            write_word(value, 4, static_cast<std::uint32_t>(
                name == linger_seconds ? linger_.ticks / ticks_per_second
                                       : linger_.ticks));
            return value;
        }
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
    static constexpr std::uint32_t socket_level = 0xffff;
    static constexpr std::uint32_t linger_ticks = 0x80;
    static constexpr std::uint32_t linger_seconds = 0x1080;
    static constexpr std::uint32_t defunct_option = 0x1100;
    // Native BSD hz, also exposed by kern.clockrate.
    static constexpr std::int32_t ticks_per_second = 100;

    static bool is_linger(std::uint32_t name)
    {
        return name == linger_ticks || name == linger_seconds;
    }

    static bool is_option_bit(std::uint32_t name)
    {
        switch (name) {
        case 0x0001: case 0x0004: case 0x0008: case 0x0010:
        case 0x0020: case 0x0040: case 0x0100: case 0x0200:
        case 0x0400: case 0x0800: case 0x2000: case 0x4000: case 0x8000:
            return true;
        default:
            return false;
        }
    }

    static std::uint32_t read_word(const Value& value, std::size_t offset)
    {
        std::uint32_t word = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
            word |= std::to_integer<std::uint32_t>(value[offset + byte]) << (byte * 8U);
        return word;
    }

    static void write_word(Value& value, std::size_t offset, std::uint32_t word)
    {
        for (std::size_t byte = 0; byte < 4; ++byte)
            value[offset + byte] = static_cast<std::byte>(word >> (byte * 8U));
    }

    struct Linger {
        bool enabled { };
        std::int32_t ticks { };
    };
    Linger linger_;
    static constexpr std::uint32_t defunct_bit = 1U << 31U;
    std::atomic<std::uint32_t> terminal_state_ { 0 };
    mutable std::mutex mutex_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Value> values_;
};

} // namespace ilemu
