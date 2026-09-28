// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "device_state/darwin_abi.hpp"
#include <cstdint>
#include <optional>

namespace ilemu::mach_transport {

// Only kernel copyin may supply CIRCULAR/RAISEIMP. Never edit user storage:
// send failures before copyin leave it intact; timeout pseudo-copyout writes
// back the normalized, ownership-adjusted kernel message separately.
class UserHeader {
public:
    [[nodiscard]] static constexpr std::optional<std::uint32_t> copyin_bits(
        std::uint32_t bits, std::uint32_t reply, DarwinMachMessageHeaderAbi abi)
    {
        constexpr std::uint32_t port_bytes_mask = 0x8000ffffU;
        constexpr std::uint32_t port_fields_mask = 0x801f1f1fU;
        if (abi == DarwinMachMessageHeaderAbi::CheckedPortBytes &&
            (bits & ~port_bytes_mask) != 0U)
            return std::nullopt;
        bits &= abi == DarwinMachMessageHeaderAbi::MaskedPortFields
                    ? port_fields_mask : port_bytes_mask;
        const auto sends_right = [](std::uint32_t disposition) {
            return disposition >= 17U && disposition <= 21U;
        };
        const auto local = (bits >> 8U) & 0xffU;
        if (!sends_right(bits & 0xffU) ||
            (local == 0U ? reply != 0U : !sends_right(local)))
            return std::nullopt;
        return bits;
    }
};

} // namespace ilemu::mach_transport
