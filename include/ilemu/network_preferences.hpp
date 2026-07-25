#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ilemu {

struct NetworkPreferencesResult {
    std::filesystem::path path;
    std::string service_identifier;
    std::vector<std::string> preferred_wifi_networks;
    bool supported{};
    bool changed{};
};

struct NetworkPreferencesIpv4 {
    std::array<std::byte, 4> address{};
    std::array<std::byte, 4> netmask{};
    std::array<std::byte, 4> gateway{};
    std::vector<std::array<std::byte, 4>> dns_servers;
};

// Ensures that the simulated device's writable SystemConfiguration state has
// a standard AirPort network service for an already-published BSD interface.
// This is a one-shot compatibility migration for root filesystems whose /var
// state predates the virtual interface; normal guest SystemConfiguration code
// owns the file after boot.
[[nodiscard]] NetworkPreferencesResult ensure_airport_network_service(
    const std::filesystem::path& rootfs, std::string_view interface_name,
    const std::array<std::byte, 6>& mac_address,
    const NetworkPreferencesIpv4& ipv4);

}  // namespace ilemu
