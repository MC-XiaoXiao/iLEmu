#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include "ilemu/display_geometry.hpp"

namespace ilemu {

// UIKit applications that declare only the phone family are presented in a
// logical phone-sized window when the device profile is a tablet. Keep this
// classification independent of bundle names and firmware versions so the
// same rule applies to every compatible legacy application.
enum class ApplicationDisplayProfileKind : std::uint8_t {
    Native,
    IPhoneCompatibility1x,
};

struct ApplicationDisplayProfile {
    ApplicationDisplayProfileKind kind {
        ApplicationDisplayProfileKind::Native
    };
    DisplayGeometry logical_geometry;
};

[[nodiscard]] ApplicationDisplayProfile detect_application_display_profile(
    const std::filesystem::path& rootfs, std::string_view executable_path,
    DisplayGeometry user_interface_geometry);

// Returns the guest-visible geometry for an application. Native applications
// retain the device output geometry; compatibility applications use their
// logical phone surface and are composed into the output at presentation.
[[nodiscard]] DisplayGeometry application_display_geometry(
    const ApplicationDisplayProfile& profile, DisplayGeometry output);

// Returns the panel rectangle occupied by the profile's logical window. A
// phone compatibility window is kept at 1x while it fits; unusual smaller
// output profiles fall back to the shared aspect-preserving fit policy.
[[nodiscard]] DisplayViewport application_display_viewport(
    const ApplicationDisplayProfile& profile, DisplayGeometry output);

// Composes a CPU scanout frame using the same compatibility viewport. This is
// used by legacy CoreSurface submissions that have no HostSurface command
// encoder; accelerated GLES submissions use the equivalent host operation.
[[nodiscard]] std::vector<std::uint32_t>
compose_application_display_pixels(const ApplicationDisplayProfile& profile,
    DisplayGeometry source, DisplayGeometry output,
    std::span<const std::uint32_t> pixels);

// Reads the bundle's declared UIKit orientation for an executable path. When
// the manifest has no orientation key, an unambiguous orientation resource
// profile may provide the capability; otherwise the legacy portrait default
// is retained. App names and versions are never used as rules.
[[nodiscard]] DisplayOrientation detect_application_display_orientation(
    const std::filesystem::path& rootfs, std::string_view executable_path);

[[nodiscard]] const char* display_orientation_name(
    DisplayOrientation orientation);

// Converts a framebuffer's top-left pixel array into the panel-facing
// orientation requested by its owner. Landscape transforms intentionally
// preserve the legacy EAGL row convention while swapping the extents.
[[nodiscard]] std::vector<std::uint32_t> orient_display_pixels(
    DisplayGeometry source_geometry, std::span<const std::uint32_t> pixels,
    DisplayOrientation orientation);

} // namespace ilemu
