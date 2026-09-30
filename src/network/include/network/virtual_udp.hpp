// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Model isolated UDP sockets, multicast membership and queued
// datagrams.

#pragma once
#include <network/socket_receive_target.hpp>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace ilemu::darwin::network { struct InterfaceSnapshot; }

namespace ilemu::bsd {

enum class VirtualUdpStatus {
    Success,
    InvalidArgument,
    AddressFamilyUnsupported,
    NotConnected,
    AlreadyConnected,
    BadFileDescriptor,
    AddressNotAvailable,
    AddressInUse,
    OptionUnsupported,
};

enum class SocketUidSharing { OwnerOnly, ExplicitOptIn };

struct VirtualUdpOptionResult {
    VirtualUdpStatus status { VirtualUdpStatus::Success };
    std::uint32_t value {};
};

struct VirtualUdpDatagram {
    std::vector<std::byte> bytes;
    std::vector<std::byte> source_address;
    std::vector<std::byte> destination_address;
    bool metadata_consumed {};
};

struct VirtualUdpAncillaryOptions {
    bool receive_destination_address { };
    bool receive_interface { };
    bool receive_hop_limit { };
    std::uint32_t interface_index { 2 };
};

[[nodiscard]] std::vector<std::byte> make_virtual_udp_ancillary(
    std::uint32_t family, const VirtualUdpDatagram& datagram,
    const VirtualUdpAncillaryOptions& options);

class VirtualUdpSocket;

// Shared in-memory UDP fabric for the isolated network policy. It models the
// BSD socket data plane without opening a host socket, so multicast services
// such as mDNS remain usable in a sandbox and between guest processes.
class VirtualUdpNetwork final
    : public std::enable_shared_from_this<VirtualUdpNetwork> {
public:
    [[nodiscard]] std::shared_ptr<VirtualUdpSocket> create(
        std::uint32_t family, std::uint32_t owner_uid = 0,
        SocketUidSharing uid_sharing = SocketUidSharing::ExplicitOptIn);
    void update_interface(const darwin::network::InterfaceSnapshot& interface);

private:
    friend class VirtualUdpSocket;

    [[nodiscard]] VirtualUdpStatus bind(
        VirtualUdpSocket& socket, std::span<const std::byte> address);
    [[nodiscard]] VirtualUdpStatus allocate_port_locked(
        const VirtualUdpSocket& socket, std::span<std::byte> address);
    [[nodiscard]] VirtualUdpStatus ensure_bound_locked(VirtualUdpSocket& socket);
    [[nodiscard]] std::shared_ptr<VirtualUdpSocket> binding_match_locked(
        std::uint32_t family, std::span<const std::byte> address, bool wildcard) const;
    [[nodiscard]] VirtualUdpStatus set_option(VirtualUdpSocket& socket,
        std::uint32_t level, std::uint32_t option,
        std::span<const std::byte> value);
    [[nodiscard]] VirtualUdpStatus send(VirtualUdpSocket& socket,
        std::span<const std::byte> bytes,
        std::span<const std::byte> destination);
    [[nodiscard]] std::optional<VirtualUdpDatagram> receive(
        VirtualUdpSocket& socket, std::size_t capacity, SocketReceiveTarget* target);
    [[nodiscard]] std::vector<std::byte> local_address(
        const VirtualUdpSocket& socket) const;
    [[nodiscard]] bool readable(const VirtualUdpSocket& socket) const;
    [[nodiscard]] std::size_t pending_bytes(
        const VirtualUdpSocket& socket) const;
    [[nodiscard]] bool broadcast_address_locked(
        std::span<const std::byte> address, std::uint32_t family) const;
    static void enqueue_locked(VirtualUdpSocket& socket,
        std::span<const std::byte> bytes, std::span<const std::byte> source,
        std::span<const std::byte> destination);

    mutable std::mutex mutex_;
    std::vector<std::weak_ptr<VirtualUdpSocket>> sockets_;
    std::uint64_t next_socket_order_ {};
    std::map<std::uint16_t, std::vector<std::array<std::byte, 4>>> broadcast_addresses_;
    static constexpr std::uint16_t first_ephemeral_port = 49'152;
    std::uint16_t next_ephemeral_port_ { first_ephemeral_port };
};

class VirtualUdpSocket final {
public:
    [[nodiscard]] std::uint32_t family() const { return family_; }
    [[nodiscard]] VirtualUdpStatus bind(std::span<const std::byte> address);
    [[nodiscard]] VirtualUdpStatus connect(std::span<const std::byte> address);
    [[nodiscard]] VirtualUdpStatus disconnect();
    void make_defunct();
    [[nodiscard]] bool defunct() const { return defunct_.load(); }
    [[nodiscard]] VirtualUdpStatus set_option(std::uint32_t level,
        std::uint32_t option, std::span<const std::byte> value);
    // Empty means the option is not owned by this backend. A recognized
    // option may still report an error for the socket's ABI contract.
    [[nodiscard]] std::optional<VirtualUdpOptionResult> get_option(
        std::uint32_t level, std::uint32_t option) const;
    [[nodiscard]] VirtualUdpStatus send(std::span<const std::byte> bytes,
        std::span<const std::byte> destination);
    [[nodiscard]] VirtualUdpStatus send(std::span<const std::byte> bytes);
    [[nodiscard]] std::optional<VirtualUdpDatagram> receive(
        std::size_t capacity, SocketReceiveTarget* target = nullptr);
    [[nodiscard]] std::vector<std::byte> local_address() const;
    [[nodiscard]] std::optional<std::vector<std::byte>> peer_address() const;
    [[nodiscard]] bool readable() const;
    [[nodiscard]] std::size_t pending_bytes() const;
    [[nodiscard]] bool writable() const { return !defunct(); }

private:
    friend class VirtualUdpNetwork;

    VirtualUdpSocket(
        std::shared_ptr<VirtualUdpNetwork> network, std::uint32_t family,
        std::uint32_t owner_uid, SocketUidSharing uid_sharing)
        : network_ { std::move(network) }
        , family_ { family }
        , owner_uid_ { owner_uid }
        , uid_sharing_ { uid_sharing }
    {
    }

    std::weak_ptr<VirtualUdpNetwork> network_;
    std::uint32_t family_ { };
    std::uint32_t owner_uid_ {};
    SocketUidSharing uid_sharing_;
    std::uint32_t reuse_options_ {};
    bool share_uid_ {};
    std::uint64_t binding_order_ {};
    std::uint64_t lookup_order_ {};
    std::vector<std::byte> bound_address_;
    std::vector<std::byte> connected_address_;
    std::set<std::vector<std::byte>> multicast_groups_;
    std::deque<VirtualUdpDatagram> incoming_;
    bool multicast_loop_ { true };
    std::atomic_bool defunct_ { false };
};

} // namespace ilemu::bsd
