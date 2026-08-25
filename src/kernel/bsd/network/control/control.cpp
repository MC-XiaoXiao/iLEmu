#include "ilemu/kernel_control.hpp"

namespace ilemu::bsd::kernel_control {

std::optional<std::uint32_t> identifier_for_name(std::string_view name)
{
    if (name == ip_interface_name)
        return ip_interface_identifier;
    return std::nullopt;
}

std::optional<std::string_view> name_for_identifier(std::uint32_t identifier)
{
    if (identifier == ip_interface_identifier)
        return ip_interface_name;
    return std::nullopt;
}

} // namespace ilemu::bsd::kernel_control
