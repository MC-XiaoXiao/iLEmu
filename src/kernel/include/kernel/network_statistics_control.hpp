/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace ilemu::bsd {

// XNU bsd/net/ntstat.{c,h}: each control connection owns its subscription
// and datagram replies. Providers are independent of the control transport.
class NetworkStatisticsControl {
public:
    std::uint32_t send(std::span<const std::byte> request, bool privileged);
    [[nodiscard]] std::span<const std::byte> next() const;
    void consume();
    [[nodiscard]] std::uint32_t pending_bytes() const;

private:
    bool sysinfo_subscribed_ { false };
    std::deque<std::vector<std::byte>> replies_;
    std::uint32_t queued_bytes_ { };
};

} // namespace ilemu::bsd
