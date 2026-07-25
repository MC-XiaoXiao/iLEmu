#pragma once

#include <cstddef>
#include <cstdint>

namespace ilemu {

// Guest-visible logical display geometry. Device profiles own the selected
// value; graphics, input, and host presentation consume the same instance.
struct DisplayGeometry {
  std::uint32_t width{};
  std::uint32_t height{};

  [[nodiscard]] constexpr bool valid() const {
    return width != 0U && height != 0U;
  }

  [[nodiscard]] constexpr std::size_t pixel_count() const {
    return static_cast<std::size_t>(width) * height;
  }
};

// Used only when no explicit device profile is supplied. It is a default
// configuration, not a renderer or frontend invariant.
inline constexpr DisplayGeometry default_display_geometry{320U, 480U};
inline constexpr std::uint32_t default_display_width =
    default_display_geometry.width;
inline constexpr std::uint32_t default_display_height =
    default_display_geometry.height;

} // namespace ilemu
