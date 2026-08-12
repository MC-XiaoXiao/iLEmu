#pragma once

#include <array>
#include <string_view>

namespace ilemu {

// SpringBoard system applications live directly under /Applications on the
// early firmware images. MobileInstallation places user applications below
// the writable data volume, using either the historical /var alias or its
// canonical /private/var spelling. Keep this boundary in one place so every
// UI/graphics/lifecycle service treats an installed application uniformly.
inline constexpr std::array<std::string_view, 3> application_path_prefixes{
    "/Applications/",
    "/var/mobile/Applications/",
    "/private/var/mobile/Applications/"};

[[nodiscard]] constexpr bool is_application_executable_path(
    std::string_view path) {
  for (const auto prefix : application_path_prefixes) {
    if (path.starts_with(prefix))
      return true;
  }
  return false;
}

} // namespace ilemu
