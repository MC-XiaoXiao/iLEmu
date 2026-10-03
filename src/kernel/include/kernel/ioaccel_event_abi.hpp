// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string_view>

namespace ilemu {
class MachOImage;

// IOAccelDevice resolves this predicate from its own firmware image. Both
// contracts consume the device's queue mapping; newer firmware tests a fixed
// array of events instead of the legacy basic event representation.
struct IOAccelEventAbi {
    std::string_view predicate { "IOAccelDeviceTestEventBasic" };

    [[nodiscard]] static std::optional<IOAccelEventAbi> detect(
        const MachOImage& image);
};
} // namespace ilemu
