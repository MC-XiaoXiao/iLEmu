#pragma once

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include "ilemu/display_geometry.hpp"

namespace ilemu {

// Reads the bundle's declared UIKit orientation for an executable path. A
// missing or malformed manifest is deliberately treated as portrait, which
// preserves the legacy firmware path and keeps orientation a capability
// discovered from the App rather than an App-name/version rule.
[[nodiscard]] DisplayOrientation detect_application_display_orientation(
    const std::filesystem::path &rootfs, std::string_view executable_path);

[[nodiscard]] const char *display_orientation_name(
    DisplayOrientation orientation);

// Converts a framebuffer's top-left pixel array into the panel-facing
// orientation requested by its owner. Landscape transforms intentionally
// preserve the legacy EAGL row convention while swapping the extents.
[[nodiscard]] std::vector<std::uint32_t> orient_display_pixels(
    DisplayGeometry source_geometry, std::span<const std::uint32_t> pixels,
    DisplayOrientation orientation);

} // namespace ilemu
