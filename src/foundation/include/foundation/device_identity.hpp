// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <string_view>

namespace ilemu {

struct DeviceIdentity {
    std::string_view product_type;
    std::string_view board_config;
    // CTL_HW/HW_MODEL identity. Keep this separate from board_config: the
    // latter is the physical IORegistry compatibility string, while an
    // activated simulator may use a development-board model so stock
    // Lockdown can take its firmware-provided no-baseband path.
    // Empty uses the physical board identity; runtime activation can supply
    // an explicit override without duplicating the normal retail identity.
    std::string_view hardware_model_override;
    // Model used by the explicit activated simulator model. This is a
    // board-specific identity; the firmware activation profile decides its use.
    std::string_view activation_hardware_model;
    // Retail configuration identifier exposed by the platform device tree.
    // This is distinct from product_type (for example, iPhone1,1) and the
    // hardware board configuration (for example, M68AP).
    std::string_view model_number;
    [[nodiscard]] constexpr std::string_view hardware_model() const noexcept
    {
        return hardware_model_override.empty() ? board_config
                                               : hardware_model_override;
    }
};

} // namespace ilemu
