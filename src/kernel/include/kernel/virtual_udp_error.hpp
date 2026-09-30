// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <network/virtual_udp.hpp>

namespace ilemu {
// Keep all BSD entry points consistent with the virtual data-plane result.
[[nodiscard]] constexpr std::uint32_t virtual_udp_error(bsd::VirtualUdpStatus status)
{
    switch (status) {
    case bsd::VirtualUdpStatus::Success:
        return 0;
    case bsd::VirtualUdpStatus::BadFileDescriptor:
        return 9; // EBADF
    case bsd::VirtualUdpStatus::InvalidArgument:
        return 22; // EINVAL
    case bsd::VirtualUdpStatus::AddressFamilyUnsupported:
        return 47; // EAFNOSUPPORT
    case bsd::VirtualUdpStatus::OptionUnsupported:
        return 42; // ENOPROTOOPT
    case bsd::VirtualUdpStatus::AddressInUse:
        return 48; // EADDRINUSE
    case bsd::VirtualUdpStatus::AddressNotAvailable:
        return 49; // EADDRNOTAVAIL: ephemeral range exhausted
    case bsd::VirtualUdpStatus::AlreadyConnected:
        return 56; // EISCONN
    case bsd::VirtualUdpStatus::NotConnected:
        return 57; // ENOTCONN
    }
    return 22;
}
} // namespace ilemu
