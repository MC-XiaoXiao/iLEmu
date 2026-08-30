#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ilemu {

// UIKit describes the logical orientation of an application's window
// independently from the panel's native framebuffer geometry. Keep this
// capability enum in the simulator boundary so host presentation and touch
// mapping share one contract without depending on UIKit or SDL types.
enum class DisplayOrientation : std::uint8_t {
    Portrait,
    PortraitUpsideDown,
    LandscapeLeft,
    LandscapeRight,
};

[[nodiscard]] constexpr bool is_landscape(DisplayOrientation orientation)
{
    return orientation == DisplayOrientation::LandscapeLeft ||
           orientation == DisplayOrientation::LandscapeRight;
}

// Guest-visible logical display geometry. Device profiles own the selected
// value; graphics, input, and host presentation consume the same instance.
struct DisplayGeometry {
    std::uint32_t width { };
    std::uint32_t height { };

    [[nodiscard]] constexpr bool valid() const
    {
        return width != 0U && height != 0U;
    }

    [[nodiscard]] constexpr std::size_t pixel_count() const
    {
        return static_cast<std::size_t>(width) * height;
    }
};

// Return the integral native-to-logical scale advertised by a device profile.
// A non-integral or invalid relationship is kept at the legacy scale so a
// custom geometry cannot silently distort input or capability data.
[[nodiscard]] constexpr std::uint32_t display_scale_factor(
    DisplayGeometry native, DisplayGeometry logical)
{
    if (!native.valid() || !logical.valid() ||
        native.width % logical.width != 0U ||
        native.height % logical.height != 0U) {
        return 1U;
    }
    const auto width_scale = native.width / logical.width;
    const auto height_scale = native.height / logical.height;
    return width_scale != 0U && width_scale == height_scale ? width_scale
                                                              : 1U;
}

struct DisplayViewport {
    std::int32_t x { };
    std::int32_t y { };
    std::uint32_t width { };
    std::uint32_t height { };
};

// Fits a guest image into a host output without changing aspect ratio.
// Upscaling prefers the largest whole-number factor; outputs smaller than the
// guest use a nearest-filtered fractional fit rather than cropping.
[[nodiscard]] constexpr DisplayViewport fit_display_viewport(
    DisplayGeometry source, DisplayGeometry output)
{
    if (!source.valid() || !output.valid())
        return { };
    const auto integer_scale =
        std::min(output.width / source.width, output.height / source.height);
    std::uint32_t width { };
    std::uint32_t height { };
    if (integer_scale != 0U) {
        width = source.width * integer_scale;
        height = source.height * integer_scale;
    } else if (static_cast<std::uint64_t>(output.width) * source.height <=
               static_cast<std::uint64_t>(output.height) * source.width) {
        width = output.width;
        height = std::max(1U, static_cast<std::uint32_t>(
                                  static_cast<std::uint64_t>(output.width) *
                                  source.height / source.width));
    } else {
        height = output.height;
        width = std::max(1U, static_cast<std::uint32_t>(
                                 static_cast<std::uint64_t>(output.height) *
                                 source.width / source.height));
    }
    return { static_cast<std::int32_t>((output.width - width) / 2U),
        static_cast<std::int32_t>((output.height - height) / 2U), width,
        height };
}

// Used only when no explicit device profile is supplied. It is a default
// configuration, not a renderer or frontend invariant.
inline constexpr DisplayGeometry default_display_geometry { 320U, 480U };
inline constexpr std::uint32_t default_display_width =
    default_display_geometry.width;
inline constexpr std::uint32_t default_display_height =
    default_display_geometry.height;

} // namespace ilemu
