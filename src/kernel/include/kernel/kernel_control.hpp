#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace ilemu::bsd::kernel_control {

inline constexpr std::string_view descriptor_kind { "system-control-socket" };
inline constexpr std::string_view ip_interface_name { "com.apple.ipif" };
inline constexpr std::uint32_t ip_interface_identifier = 1;

struct Address {
    std::uint32_t identifier { };
    std::uint32_t unit { };
};

struct Endpoint {
    std::uint32_t socket_type { };
    std::optional<Address> peer;
    std::uint64_t transmitted_bytes { };
};

[[nodiscard]] std::optional<std::uint32_t> identifier_for_name(
    std::string_view name);
[[nodiscard]] std::optional<std::string_view> name_for_identifier(
    std::uint32_t identifier);

} // namespace ilemu::bsd::kernel_control
