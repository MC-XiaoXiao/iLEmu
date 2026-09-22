// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Select a guest graphics driver provided by the firmware itself.

#include "foundation/device_display_profile.hpp"

#include <system_error>

namespace ilemu {

std::string DeviceDisplayProfile::resolve_driver_bundle(
    const std::filesystem::path& rootfs) const
{
    const std::string preferred { driver_bundle() };
    if (preferred.empty())
        return preferred;

    const auto extensions = rootfs / "System/Library/Extensions";
    const auto has_info = [](const std::filesystem::path& bundle) {
        std::error_code error;
        return std::filesystem::is_regular_file(bundle / "Info.plist", error);
    };
    if (has_info(extensions / (preferred + ".bundle")))
        return preferred;

    constexpr std::string_view suffix = "GLDriver";
    const auto family = preferred.substr(0, preferred.size() - suffix.size());
    std::error_code error;
    std::string available;
    for (std::filesystem::directory_iterator it { extensions, error }, end;
        !error && it != end; it.increment(error)) {
        if (it->path().extension() != ".bundle" || !has_info(it->path()))
            continue;
        const auto name = it->path().stem().string();
        if (!name.starts_with(family) || !name.ends_with(suffix))
            continue;
        if (!available.empty())
            return preferred; // Ambiguous firmware: do not guess a revision.
        available = name;
    }
    return !error && !available.empty() ? available : preferred;
}

} // namespace ilemu
