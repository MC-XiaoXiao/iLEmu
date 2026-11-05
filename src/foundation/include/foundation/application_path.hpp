// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Classify system and data-volume application bundle paths.

#pragma once

#include <array>
#include <string_view>

namespace ilemu {

// SpringBoard system applications live directly under /Applications on the
// early firmware images. MobileInstallation places user applications below
// the writable data volume, using either the historical /var alias or its
// canonical /private/var spelling. Keep this boundary in one place so every
// UI/graphics/lifecycle service treats an installed application uniformly.
inline constexpr std::array<std::string_view, 3> application_path_prefixes {
    "/Applications/", "/var/mobile/Applications/",
    "/private/var/mobile/Applications/"
};

[[nodiscard]] constexpr bool is_application_path(std::string_view path)
{
    for (const auto prefix : application_path_prefixes) {
        if (path.starts_with(prefix))
            return true;
    }
    return false;
}

[[nodiscard]] constexpr bool is_application_executable_path(
    std::string_view path)
{
    if (!is_application_path(path))
        return false;
    // An iOS application's executable is at the top of its .app bundle.
    // Embedded extensions, frameworks and helpers belong to that installation
    // but are not the containing application's fullscreen/event-route owner.
    const auto bundle = path.find(".app/");
    return bundle != std::string_view::npos && bundle + 5U < path.size() &&
           path.find('/', bundle + 5U) == std::string_view::npos;
}

// The setup assistant is a system-owned display client during foreground
// handoff, before SpringBoard has published its replacement scene.
[[nodiscard]] constexpr bool is_setup_assistant_executable_path(
    std::string_view path)
{
    return path == "/Applications/Setup.app/Setup";
}

} // namespace ilemu
