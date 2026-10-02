// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <network/inet_address.hpp>
#include <network/darwin_socket_abi.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace ilemu::bsd {
class IsolatedStreamSocket;

// Isolated streams reserve guest ports without opening host sockets. Registry
// entries are weak: only guest open descriptions and in-flight rights own them.
class IsolatedStreamNetwork final
    : public std::enable_shared_from_this<IsolatedStreamNetwork> {
public:
    [[nodiscard]] std::shared_ptr<IsolatedStreamSocket> create(
        std::uint32_t family, std::uint32_t owner_uid);
private:
    friend class IsolatedStreamSocket;
    [[nodiscard]] std::shared_ptr<IsolatedStreamSocket> match_locked(
        std::uint32_t family, std::span<const std::byte> address, bool wildcard);
    [[nodiscard]] std::uint32_t bind_locked(IsolatedStreamSocket& socket,
        std::span<const std::byte> address);
    std::mutex mutex_;
    std::map<std::uint32_t, std::vector<std::weak_ptr<IsolatedStreamSocket>>> bindings_;
    std::uint16_t next_port_ { 49'152 };
};

class IsolatedStreamSocket final
    : public std::enable_shared_from_this<IsolatedStreamSocket> {
public:
    IsolatedStreamSocket(const IsolatedStreamSocket&) = delete;
    IsolatedStreamSocket& operator=(const IsolatedStreamSocket&) = delete;
    [[nodiscard]] std::uint32_t bind(std::span<const std::byte> address)
    {
        std::lock_guard lock { network_->mutex_ };
        return network_->bind_locked(*this, address);
    }
    [[nodiscard]] std::uint32_t listen()
    {
        std::lock_guard lock { network_->mutex_ };
        if (defunct_)
            return 22; // EINVAL
        if (bound_address_.empty()) {
            const auto address = unnamed_address();
            if (const auto error = network_->bind_locked(*this, address))
                return error;
        }
        listening_ = true;
        return 0;
    }
    // sodefunct closes a pre-established TCP PCB even while descriptors
    // still own the socket. Retain its name and SO_ACCEPTCONN, but exclude
    // it from reservation lookup. Guest so_error belongs to the shared
    // socket state above this transport and is consumed by the kernel.
    void make_defunct()
    {
        std::lock_guard lock { network_->mutex_ };
        if (defunct_)
            return;
        defunct_ = true;
    }
    [[nodiscard]] bool defunct() const
    {
        std::lock_guard lock { network_->mutex_ };
        return defunct_;
    }
    [[nodiscard]] std::uint32_t connect_error() const
    {
        std::lock_guard lock { network_->mutex_ };
        return defunct_ || listening_ ? 45U : 51U; // EOPNOTSUPP / ENETUNREACH
    }
    [[nodiscard]] bool listening() const
    {
        std::lock_guard lock { network_->mutex_ };
        return listening_;
    }
    [[nodiscard]] std::vector<std::byte> local_address() const
    {
        std::lock_guard lock { network_->mutex_ };
        return bound_address_.empty() ? unnamed_address() : bound_address_;
    }
    void set_option(std::uint32_t level, std::uint32_t option,
        std::span<const std::byte> value)
    {
        using namespace darwin::socket;
        if (level != option_level || value.size() < 4 ||
            (option != option_reuse_address && option != option_reuse_port &&
                option != option_reuse_share_uid))
            return;
        const bool enabled = std::any_of(value.begin(), value.begin() + 4,
            [](std::byte byte) { return byte != std::byte { 0 }; });
        std::lock_guard lock { network_->mutex_ };
        if (option == option_reuse_share_uid)
            share_uid_ = enabled;
        else if (enabled)
            reuse_options_ |= option;
        else
            reuse_options_ &= ~option;
    }
private:
    friend class IsolatedStreamNetwork;
    IsolatedStreamSocket(std::shared_ptr<IsolatedStreamNetwork> network,
        std::uint32_t family, std::uint32_t owner_uid)
        : network_ { std::move(network) }, family_ { family }, owner_uid_ { owner_uid } {}
    [[nodiscard]] std::vector<std::byte> unnamed_address() const
    {
        std::vector<std::byte> address(inet_address::address_size(family_));
        address[0] = static_cast<std::byte>(address.size());
        address[1] = static_cast<std::byte>(family_);
        return address;
    }
    std::shared_ptr<IsolatedStreamNetwork> network_;
    const std::uint32_t family_;
    const std::uint32_t owner_uid_;
    std::uint32_t reuse_options_ {};
    bool share_uid_ {};
    bool listening_ {};
    bool defunct_ {};
    std::vector<std::byte> bound_address_;
};

inline std::shared_ptr<IsolatedStreamSocket> IsolatedStreamNetwork::create(
    std::uint32_t family, std::uint32_t owner_uid)
{
    return std::shared_ptr<IsolatedStreamSocket> {
        new IsolatedStreamSocket { shared_from_this(), family, owner_uid } };
}

inline std::shared_ptr<IsolatedStreamSocket> IsolatedStreamNetwork::match_locked(
    std::uint32_t family, std::span<const std::byte> address, bool wildcard)
{
    using namespace inet_address;
    const auto port = (std::to_integer<std::uint32_t>(address[2]) << 8U) |
                      std::to_integer<std::uint32_t>(address[3]);
    const auto entry = bindings_.find((family << 16U) | port);
    if (entry == bindings_.end())
        return {};
    auto& candidates = entry->second;
    std::erase_if(candidates, [](const auto& weak) {
        const auto socket = weak.lock();
        return !socket || socket->defunct_;
    });
    if (candidates.empty()) {
        bindings_.erase(entry);
        return {};
    }
    std::shared_ptr<IsolatedStreamSocket> best;
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        const auto other = it->lock();
        if (!other)
            continue;
        const auto& bound = other->bound_address_;
        if (same_ip(address, bound, family))
            return other;
        if (!best && wildcard &&
            (wildcard_address(address, family) || wildcard_address(bound, family)))
            best = other;
    }
    return best;
}

inline std::uint32_t IsolatedStreamNetwork::bind_locked(
    IsolatedStreamSocket& socket, std::span<const std::byte> address)
{
    using namespace inet_address;
    using namespace darwin::socket;
    if (socket.defunct_ || !socket.bound_address_.empty() ||
        !valid_address(address, socket.family_))
        return 22; // EINVAL
    auto bound = normalize_address(address, socket.family_);
    if (zero_port(bound)) {
        constexpr unsigned port_count = 65'536U - 49'152U;
        bool allocated = false;
        for (unsigned attempt = 0; attempt < port_count; ++attempt) {
            set_port(bound, next_port_++);
            if (next_port_ == 0)
                next_port_ = 49'152;
            if (!match_locked(socket.family_, bound, socket.reuse_options_ == 0)) {
                allocated = true;
                break;
            }
        }
        if (!allocated)
            return 49; // EADDRNOTAVAIL
    } else {
        // in_pcbbind checks UID ownership before option-based PCB lookup.
        if (socket.owner_uid_ != 0 && !multicast_address(bound, socket.family_)) {
            const auto other = match_locked(socket.family_, bound, true);
            if (other && socket.owner_uid_ != other->owner_uid_ && !other->share_uid_ &&
                (!wildcard_address(bound, socket.family_) ||
                    !wildcard_address(other->bound_address_, socket.family_) ||
                    !(other->reuse_options_ & option_reuse_port)))
                return 48; // EADDRINUSE
        }
        const auto other = match_locked(socket.family_, bound, socket.reuse_options_ == 0);
        auto reuse = socket.reuse_options_ & option_reuse_port;
        if (multicast_address(bound, socket.family_) &&
            (socket.reuse_options_ & option_reuse_address))
            reuse = option_reuse_address | option_reuse_port;
        if (other && !(reuse & other->reuse_options_))
            return 48;
    }
    const auto port = (std::to_integer<std::uint32_t>(bound[2]) << 8U) |
                      std::to_integer<std::uint32_t>(bound[3]);
    socket.bound_address_ = std::move(bound);
    bindings_[(socket.family_ << 16U) | port].push_back(socket.weak_from_this());
    return 0;
}
} // namespace ilemu::bsd
