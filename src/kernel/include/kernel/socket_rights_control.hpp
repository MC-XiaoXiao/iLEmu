// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <network/darwin_abi_route.hpp>
#include <network/darwin_socket_abi.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ilemu {
// ARM32 SCM_RIGHTS output. Importing descriptors belongs to soreceive,
// before uiomove; copying this wire data belongs to recvit, after payload.
class SocketRightsControl {
public:
    static bool externalizes(DarwinAbiEpoch epoch, bool requested)
    {
        // xnu-792 externalizes only with controlp; xnu-1228+ does so
        // even when the caller discards the ancillary output.
        return requested || (epoch != DarwinAbiEpoch::IphoneOs1 &&
                             epoch != DarwinAbiEpoch::Unknown);
    }
    template <typename Transfers, typename Available, typename Import>
    std::uint32_t externalize(const Transfers& transfers, DarwinAbiEpoch epoch,
        bool requested, std::uint32_t limit, Available&& available, Import&& import)
    {
        if (!externalizes(epoch, requested))
            return 0;
        // unp_externalize checks fdavail for the complete rights mbuf.
        // The caller's kernel mutex protects this check and all imports.
        std::size_t free = 0;
        for (std::uint32_t fd = 3; fd < limit && free < transfers.size(); ++fd)
            if (available(fd))
                ++free;
        if (free < transfers.size())
            return 40; // EMSGSIZE; never partially import a descriptor batch
        if (requested)
            begin(transfers.size());
        for (const auto& transfer : transfers) {
            const auto descriptor = import(transfer);
            if (!descriptor)
                return 40;
            if (requested)
                append(*descriptor);
            ++imported_;
        }
        return 0;
    }
    [[nodiscard]] std::size_t imported() const { return imported_; }
    void begin(std::size_t descriptors)
    {
        ends_.push_back(bytes_.size() + 12U + descriptors * 4U);
        bytes_.reserve(ends_.back());
        append(static_cast<std::uint32_t>(12U + descriptors * 4U));
        append(darwin::socket::option_level);
        append(1U); // SCM_RIGHTS
    }
    void append(std::uint32_t word)
    {
        for (unsigned byte = 0; byte < 4; ++byte)
            bytes_.push_back(static_cast<std::byte>(word >> (byte * 8U)));
    }
    [[nodiscard]] std::span<const std::byte> prefix(std::size_t capacity) const
    {
        return std::span<const std::byte> {bytes_}.first(std::min(capacity, bytes_.size()));
    }
    [[nodiscard]] std::uint32_t flags(std::size_t capacity) const
    {
        // recvit only visits control mbufs while len > 0. It copies a
        // byte prefix, including a partial cmsghdr, not whole fd slots.
        return capacity != 0 && capacity < bytes_.size() &&
               std::find(ends_.begin(), ends_.end(), capacity) == ends_.end()
            ? darwin::socket::message_control_truncated : 0U;
    }
private:
    std::size_t imported_ {};
    std::vector<std::byte> bytes_;
    std::vector<std::size_t> ends_;
};
} // namespace ilemu
