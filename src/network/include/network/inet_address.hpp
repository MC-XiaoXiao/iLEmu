// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//

#pragma once
#include <network/darwin_network_abi.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ilemu::bsd::inet_address {
    using namespace darwin::network;
    [[nodiscard]] inline std::size_t address_size(std::uint32_t family)
    {
        if (family == address_family_inet)
            return arm32_sockaddr_in_size;
        if (family == address_family_inet6)
            return arm32_sockaddr_in6_size;
        return 0;
    }

    [[nodiscard]] inline bool valid_address(
        std::span<const std::byte> address, std::uint32_t family)
    {
        const auto expected = address_size(family);
        return expected != 0 && address.size() >= expected &&
               std::to_integer<std::uint8_t>(address[sockaddr_family_offset]) ==
                   family;
    }

    [[nodiscard]] inline std::vector<std::byte> normalize_address(
        std::span<const std::byte> address, std::uint32_t family)
    {
        const auto size = address_size(family);
        std::vector<std::byte> normalized(
            address.begin(), address.begin() + size);
        normalized[sockaddr_length_offset] = static_cast<std::byte>(size);
        return normalized;
    }

    [[nodiscard]] inline bool same_port(
        std::span<const std::byte> left, std::span<const std::byte> right)
    {
        return left[sockaddr_port_offset] == right[sockaddr_port_offset] &&
               left[sockaddr_port_offset + 1U] ==
                   right[sockaddr_port_offset + 1U];
    }

    [[nodiscard]] inline bool zero_port(std::span<const std::byte> address)
    {
        return address[sockaddr_port_offset] == std::byte { 0 } &&
               address[sockaddr_port_offset + 1U] == std::byte { 0 };
    }

    inline void set_port(std::span<std::byte> address, std::uint16_t port)
    {
        address[sockaddr_port_offset] = static_cast<std::byte>(port >> 8U);
        address[sockaddr_port_offset + 1U] = static_cast<std::byte>(port);
    }

    [[nodiscard]] inline std::span<const std::byte> address_bytes(
        std::span<const std::byte> address, std::uint32_t family)
    {
        const auto offset = family == address_family_inet
                                ? sockaddr_ipv4_address_offset
                                : sockaddr_ipv6_address_offset;
        const auto count = family == address_family_inet ? 4U : 16U;
        return address.subspan(offset, count);
    }

    [[nodiscard]] inline bool wildcard_address(
        std::span<const std::byte> address, std::uint32_t family)
    {
        const auto bytes = address_bytes(address, family);
        return std::all_of(bytes.begin(), bytes.end(),
            [](std::byte value) { return value == std::byte { 0 }; });
    }

    [[nodiscard]] inline bool multicast_address(
        std::span<const std::byte> address, std::uint32_t family)
    {
        const auto bytes = address_bytes(address, family);
        if (family == address_family_inet) {
            const auto first = std::to_integer<std::uint8_t>(bytes.front());
            return first >= 224U && first <= 239U;
        }
        return bytes.front() == std::byte { 0xff };
    }

    [[nodiscard]] inline bool same_ip(std::span<const std::byte> left,
        std::span<const std::byte> right, std::uint32_t family)
    {
        return std::ranges::equal(
            address_bytes(left, family), address_bytes(right, family));
    }


} // namespace ilemu::bsd::inet_address
