// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "foundation/device_identity.hpp"

namespace ilemu {

// Selected by the firmware version, independently of physical device identity
// and kernel wire layouts. The board-specific identity string remains device
// data.
struct FirmwareActivationProfile {
    enum class Identity { Retail, DevelopmentBoard };
    Identity identity { Identity::Retail };

    [[nodiscard]] constexpr std::string_view name() const
    {
        return identity == Identity::DevelopmentBoard ? "development-board"
                                                      : "retail";
    }

    constexpr void apply(DeviceIdentity& device, bool activated) const
    {
        if (activated && identity == Identity::DevelopmentBoard)
            device.hardware_model_override = device.activation_hardware_model;
    }
};

} // namespace ilemu
