// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ilemu {

// ABI boundaries use the numeric generation and, only within a generation,
// the release train. Revision numbers and beta suffixes do not select an ABI.
struct DarwinBuildVersion {
    std::uint32_t generation;
    // Zero denotes a major-only boundary, e.g. { 10U } compares only 10.
    char train { };

    [[nodiscard]] constexpr bool at_least(DarwinBuildVersion minimum) const
    {
        if (generation != minimum.generation)
            return generation > minimum.generation;
        return minimum.train == 0 || train >= minimum.train;
    }

    auto operator<=>(const DarwinBuildVersion&) const = default;
    [[nodiscard]] static std::optional<DarwinBuildVersion> parse(
        std::string_view build);
};

} // namespace ilemu
