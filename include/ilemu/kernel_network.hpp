#pragma once

#include <string_view>

#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/kernel_shared_state.hpp"

namespace ilemu::kernel_network {

[[nodiscard]] darwin::network::InterfaceSnapshot make_interface_snapshot(
    std::string_view name,
    const KernelSharedState::NetworkInterface& interface);

}  // namespace ilemu::kernel_network
