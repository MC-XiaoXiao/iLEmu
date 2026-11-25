// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ilemu {
class MachOImage;
enum class KeyboardWireProfile {
    Unavailable,
    ScalarCompact,
    ScalarExtended,
    Utf16Extended
};
// Encodes committed text for the firmware's GraphicsServices key events.
// Window-system composition belongs to the host; UIKit owns text editing.
class GraphicsServicesKeyboard {
public:
    using Payload = std::vector<std::byte>;
    [[nodiscard]] static std::optional<std::vector<Payload>> encode(
        std::string_view utf8, KeyboardWireProfile profile);
    [[nodiscard]] static KeyboardWireProfile detect(const MachOImage& image);
};
} // namespace ilemu
