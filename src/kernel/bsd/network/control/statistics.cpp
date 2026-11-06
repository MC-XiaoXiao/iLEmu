/* SPDX-License-Identifier: MPL-2.0 */
#include "kernel/network_statistics_control.hpp"

#include <algorithm>
#include <limits>

namespace ilemu::bsd {
namespace {
    constexpr std::uint32_t header_size = 16;
    constexpr std::uint32_t receive_space = 8192;

    std::uint64_t load(std::span<const std::byte> bytes,
        std::size_t offset, std::size_t count)
    {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < count; ++i)
            value |= std::uint64_t(std::to_integer<unsigned>(bytes[offset + i])) << (i * 8);
        return value;
    }

    void store(std::span<std::byte> bytes, std::size_t offset,
        std::uint64_t value, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
            bytes[offset + i] = static_cast<std::byte>(value >> (i * 8));
    }
}

std::uint32_t NetworkStatisticsControl::send(
    std::span<const std::byte> request, bool privileged)
{
    // nstat_control_send rejects a short header synchronously; protocol
    // errors after that are delivered as messages with the request context.
    if (request.size() < header_size)
        return 22; // EINVAL
    const auto type = load(request, 8, 4);
    std::uint32_t error = 22;
    bool success_reply = false;
    switch (type) {
    case 1008: // SUBSCRIBE_SYSINFO has no acknowledgement in XNU.
        if (privileged) {
            sysinfo_subscribed_ = true;
            return 0;
        }
        error = 1; // EPERM
        break;
    case 1001: // ADD_SRC: no statistics providers registered yet.
        error = request.size() >= 20 ? 2 : 22; // ENOENT / EINVAL
        break;
    case 1002: // ADD_ALL_SRCS
        error = request.size() >= 56 ? 2 : 22;
        break;
    case 1003: // REM_SRC
    case 1006: // SET_FILTER
        error = request.size() >= (type == 1006 ? 28U : 24U) ? 2 : 22;
        break;
    case 1004: // QUERY_SRC
    case 1005: // GET_SRC_DESC
    case 1007: // GET_UPDATE
        if (request.size() >= 24) {
            success_reply = load(request, 16, 8) ==
                std::numeric_limits<std::uint64_t>::max();
            error = success_reply ? 0 : 2;
        }
        break;
    }
    const auto length = success_reply ? header_size :
        header_size + 4U + request.size();
    if (length > receive_space - queued_bytes_)
        return 55; // ENOBUFS, as ctl_enqueuedata on a full receive buffer.
    std::vector<std::byte> reply(length);
    store(reply, 0, load(request, 0, 8), 8);
    store(reply, 8, success_reply ? 0 : 1, 4);
    store(reply, 12, length, 2);
    if (!success_reply) {
        store(reply, 16, error, 4);
        std::copy(request.begin(), request.end(), reply.begin() + 20);
        // XNU normalizes legacy headers before returning the failed request.
        if (load(request, 12, 2) != request.size()) {
            store(reply, 32, request.size(), 2);
            store(reply, 34, 0, 2);
        }
    }
    queued_bytes_ += static_cast<std::uint32_t>(reply.size());
    replies_.push_back(std::move(reply));
    return 0;
}

std::span<const std::byte> NetworkStatisticsControl::next() const
{
    return replies_.empty() ? std::span<const std::byte> {} : replies_.front();
}

void NetworkStatisticsControl::consume()
{
    queued_bytes_ -= static_cast<std::uint32_t>(replies_.front().size());
    replies_.pop_front();
}

std::uint32_t NetworkStatisticsControl::pending_bytes() const
{
    return queued_bytes_;
}

} // namespace ilemu::bsd
