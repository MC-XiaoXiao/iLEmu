#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ilemu::virtual_network {

using Ipv4Address = std::array<std::byte, 4>;
using MacAddress = std::array<std::byte, 6>;

struct Ipv4Neighbor {
    Ipv4Address address;
    MacAddress mac_address;
};

inline constexpr MacAddress interface_mac_address { std::byte { 0x02 },
    std::byte { 0x1a }, std::byte { 0x54 }, std::byte { 0x3a },
    std::byte { 0x00 }, std::byte { 0x02 } };
inline constexpr std::string_view access_point_ssid { "iLEmu" };
inline constexpr std::uint16_t access_point_channel { 6 };

inline constexpr Ipv4Address client_address { std::byte { 10 }, std::byte { 0 },
    std::byte { 2 }, std::byte { 15 } };
inline constexpr Ipv4Address netmask { std::byte { 255 }, std::byte { 255 },
    std::byte { 255 }, std::byte { 0 } };
inline constexpr Ipv4Address broadcast_address { std::byte { 10 },
    std::byte { 0 }, std::byte { 2 }, std::byte { 255 } };
inline constexpr Ipv4Address gateway_address { std::byte { 10 },
    std::byte { 0 }, std::byte { 2 }, std::byte { 2 } };
inline constexpr MacAddress gateway_mac_address { std::byte { 0x02 },
    std::byte { 0x1a }, std::byte { 0x54 }, std::byte { 0x3a },
    std::byte { 0x00 }, std::byte { 0x01 } };
inline constexpr std::array ipv4_neighbors { Ipv4Neighbor {
    gateway_address, gateway_mac_address } };
inline constexpr Ipv4Address dns_proxy_address { std::byte { 10 },
    std::byte { 0 }, std::byte { 2 }, std::byte { 3 } };
inline constexpr std::uint16_t dns_port = 53;

} // namespace ilemu::virtual_network
