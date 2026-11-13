/* SPDX-License-Identifier: MPL-2.0 */
#include "network/necp_policy.hpp"

#include <algorithm>
#include <string>

namespace ilemu::darwin::necp {
namespace {

std::uint32_t word(std::span<const std::byte> bytes)
{
    std::uint32_t result { };
    for (unsigned i = 0; i < 4; ++i)
        result |= std::to_integer<std::uint32_t>(bytes[i]) << (i * 8U);
    return result;
}

void put_word(std::span<std::byte> bytes, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = static_cast<std::byte>(value >> (i * 8U));
}

void tlv(Bytes& result, std::uint8_t type, std::span<const std::byte> value)
{
    const auto start = result.size();
    result.resize(start + 5U + value.size());
    result[start] = static_cast<std::byte>(type);
    put_word(std::span { result }.subspan(start + 1U, 4U),
        static_cast<std::uint32_t>(value.size()));
    std::copy(value.begin(), value.end(), result.begin() +
        static_cast<std::ptrdiff_t>(start + 5U));
}

void tlv_word(Bytes& result, std::uint8_t type, std::uint32_t value)
{
    std::array<std::byte, 4> bytes;
    put_word(bytes, value);
    tlv(result, type, bytes);
}

std::string text(std::span<const std::byte> bytes)
{
    const auto end = std::find(bytes.begin(), bytes.end(), std::byte { });
    return { reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::size_t>(end - bytes.begin()) };
}

} // namespace

Bytes evaluate(const ClientId& id, std::span<const std::byte> parameters,
    std::span<const Interface> interfaces, const route::Table& routes)
{
    std::string bound;
    std::vector<std::string> prohibited_names;
    std::vector<std::uint8_t> prohibited_types;
    std::vector<std::uint8_t> required_types;
    bool required_agent { };
    Bytes destination;
    while (!parameters.empty()) {
        if (parameters.size() < 5U)
            return { };
        const auto type = std::to_integer<unsigned>(parameters[0]);
        const auto size = word(parameters.subspan(1U, 4U));
        if (size > parameters.size() - 5U)
            return { };
        const auto value = parameters.subspan(5U, size);
        switch (type) {
        case 9: bound = text(value); break;
        case 100: prohibited_names.push_back(text(value)); break;
        case 101:
            if (!value.empty())
                prohibited_types.push_back(std::to_integer<std::uint8_t>(value[0]));
            break;
        case 111:
            if (!value.empty())
                required_types.push_back(std::to_integer<std::uint8_t>(value[0]));
            break;
        case 112:
        case 113: required_agent = true; break;
        case 13:
            if (value.size() >= 29U) // packed prefix + sockaddr union
                destination.assign(value.begin() + 1, value.end());
            break;
        case 201: destination.assign(value.begin(), value.end()); break;
        default: break;
        }
        parameters = parameters.subspan(5U + size);
    }
    const auto matches = [&](const Interface& interface) {
        const auto& address = interface.address;
        return !required_agent &&
            (bound.empty() || bound == address.name) &&
            (required_types.empty() ||
                std::find(required_types.begin(), required_types.end(),
                    interface.functional_type) != required_types.end()) &&
            std::find(prohibited_names.begin(), prohibited_names.end(), address.name) ==
                prohibited_names.end() &&
            std::find(prohibited_types.begin(), prohibited_types.end(),
                interface.functional_type) == prohibited_types.end() &&
            (address.flags & network::interface_flag_up) != 0U;
    };
    route::Table eligible_routes;
    for (const auto& entry : routes.snapshot()) {
        const auto interface = std::find_if(interfaces.begin(), interfaces.end(),
            [&](const Interface& value) {
                return (entry.interface_index == value.address.index ||
                    entry.interface_name == value.address.name) && matches(value);
            });
        if (interface != interfaces.end() && (entry.flags & route::flag_up) != 0U)
            static_cast<void>(eligible_routes.apply(route::message_add, entry));
    }
    const auto lookup = [&](std::uint8_t family) {
        route::Entry query;
        query.family = family;
        query.destination = destination;
        if (query.destination.size() < 2U ||
            std::to_integer<unsigned>(query.destination[1]) != family) {
            query.destination.assign(family == network::address_family_inet ? 16U : 28U,
                std::byte { });
            query.destination[0] = static_cast<std::byte>(query.destination.size());
            query.destination[1] = static_cast<std::byte>(family);
        }
        return eligible_routes.lookup(query);
    };
    const auto family = destination.size() >= 2U
        ? std::to_integer<unsigned>(destination[1]) : 0U;
    auto selected = family == network::address_family_inet6
        ? lookup(network::address_family_inet6) : lookup(network::address_family_inet);
    if (!selected && family == 0U)
        selected = lookup(network::address_family_inet6);
    const auto interface = selected
        ? std::find_if(interfaces.begin(), interfaces.end(), [&](const Interface& value) {
              return selected->interface_index == value.address.index ||
                  selected->interface_name == value.address.name;
          })
        : interfaces.end();
    Bytes result;
    tlv(result, 1U, id);
    const bool scoped = interface != interfaces.end() &&
        (!bound.empty() || !required_types.empty());
    tlv_word(result, 2U, scoped ? 12U : 0U);
    if (interface != interfaces.end()) {
        const auto& address = interface->address;
        if (scoped)
            tlv_word(result, 3U, address.index);
        tlv_word(result, 5U, address.index);
        const auto flags =
            (((selected->flags & route::flag_local) != 0U ||
                (address.flags & network::interface_flag_loopback) != 0U) ? 1U : 0U) |
            ((selected->flags & route::flag_gateway) == 0U ? 2U : 0U) |
            (address.ipv4_address ? 4U : 0U) | (address.ipv6_address ? 8U : 0U);
        if (flags != 0U)
            tlv_word(result, 7U, flags);
        std::array<std::byte, 8> identity { }; // stable virtual interface generation
        put_word(std::span { identity }.subspan(4U, 4U), address.index);
        tlv(result, 8U, identity);
    }
    return result;
}

Bytes interface_details(std::uint32_t index, std::span<const Interface> interfaces)
{
    // necp_interface_details: IFXNAMSIZ + six words + two 20-byte signatures.
    Bytes result(80);
    const auto interface = std::find_if(interfaces.begin(), interfaces.end(),
        [&](const Interface& value) { return value.address.index == index; });
    if (interface == interfaces.end())
        return result;
    const auto& address = interface->address;
    std::transform(address.name.begin(),
        address.name.begin() + static_cast<std::ptrdiff_t>(
            std::min<std::size_t>(address.name.size(), 15U)), result.begin(),
        [](char value) { return static_cast<std::byte>(value); });
    put_word(std::span { result }.subspan(16U, 4U), address.index);
    put_word(std::span { result }.subspan(24U, 4U), interface->functional_type);
    put_word(std::span { result }.subspan(36U, 4U), address.mtu);
    return result;
}

} // namespace ilemu::darwin::necp
