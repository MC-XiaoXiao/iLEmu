/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "network/necp_client.hpp"
#include "network/darwin_network_abi.hpp"
#include "network/darwin_route_socket.hpp"

namespace ilemu::darwin::necp {

struct Interface {
    network::InterfaceSnapshot address;
    std::uint32_t functional_type;
};

// XNU3789 client results use packed type:u8,length:u32,value TLVs. Route
// selection reuses the guest route table, including its longest-prefix match.
[[nodiscard]] Bytes evaluate(const ClientId& id,
    std::span<const std::byte> parameters, std::span<const Interface> interfaces,
    const route::Table& routes);
[[nodiscard]] Bytes interface_details(std::uint32_t index,
    std::span<const Interface> interfaces);

} // namespace ilemu::darwin::necp
