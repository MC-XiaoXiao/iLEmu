// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "device_state/darwin_build_version.hpp"

#include <charconv>

namespace ilemu {

std::optional<DarwinBuildVersion> DarwinBuildVersion::parse(
    std::string_view build)
{
    constexpr std::string_view digits { "0123456789" };
    const auto branch = build.find_first_not_of(digits);
    if (branch == 0 || branch == std::string_view::npos ||
        build[branch] < 'A' || build[branch] > 'Z')
        return std::nullopt;
    std::uint32_t generation { };
    const auto parsed =
        std::from_chars(build.data(), build.data() + branch, generation);
    if (parsed.ec != std::errc { } || generation == 0)
        return std::nullopt;
    const auto revision = build.substr(branch + 1);
    if (revision.empty())
        return std::nullopt;
    const auto suffix = revision.find_first_not_of(digits);
    if (suffix != std::string_view::npos &&
        !(suffix > 0 && suffix == revision.size() - 1 &&
            revision.back() >= 'a' && revision.back() <= 'z'))
        return std::nullopt;
    return DarwinBuildVersion { generation, build[branch] };
}

} // namespace ilemu
