#pragma once

#include <string_view>

#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/kernel_shared_state.hpp"

namespace ilemu::kernel_network {

// An isolated guest still has a local IP stack. Stream endpoints in this
// mode deliberately have no HostSocket: bind/listen state remains inside the
// simulator, while connect cannot escape to a host or external address.
inline constexpr std::string_view isolated_ipv4_stream_descriptor {
    "isolated-inet-stream"
};
inline constexpr std::string_view isolated_ipv6_stream_descriptor {
    "isolated-inet6-stream"
};

[[nodiscard]] constexpr bool is_isolated_stream_descriptor(
    std::string_view descriptor)
{
    return descriptor == isolated_ipv4_stream_descriptor ||
           descriptor == isolated_ipv6_stream_descriptor;
}

[[nodiscard]] darwin::network::InterfaceSnapshot make_interface_snapshot(
    std::string_view name,
    const KernelSharedState::NetworkInterface& interface);

} // namespace ilemu::kernel_network
