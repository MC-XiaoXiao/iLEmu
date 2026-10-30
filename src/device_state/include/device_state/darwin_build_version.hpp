// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ilemu {

// Validate a complete build code but retain only its numeric major generation.
// Release trains, revisions and beta suffixes never select compatibility.
struct DarwinBuildVersion {
    std::uint32_t generation;
    auto operator<=>(const DarwinBuildVersion&) const = default;
    [[nodiscard]] static std::optional<DarwinBuildVersion> parse(
        std::string_view build);
};

} // namespace ilemu
