#include "ilemu/application_display_profile.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#if defined(ILEMU_HAS_LIBPLIST)
#include <plist/plist.h>
#endif

namespace ilemu {
namespace {

std::optional<DisplayOrientation> parse_orientation(std::string_view value) {
  if (value == "UIInterfaceOrientationPortrait")
    return DisplayOrientation::Portrait;
  if (value == "UIInterfaceOrientationPortraitUpsideDown")
    return DisplayOrientation::PortraitUpsideDown;
  if (value == "UIInterfaceOrientationLandscapeLeft")
    return DisplayOrientation::LandscapeLeft;
  if (value == "UIInterfaceOrientationLandscapeRight")
    return DisplayOrientation::LandscapeRight;
  return std::nullopt;
}

std::filesystem::path info_plist_path(const std::filesystem::path &rootfs,
                                      std::string_view executable_path) {
  auto relative = std::filesystem::path{executable_path};
  if (relative.is_absolute())
    relative = relative.relative_path();
  return rootfs / relative.parent_path() / "Info.plist";
}

#if defined(ILEMU_HAS_LIBPLIST)
class PlistOwner {
public:
  explicit PlistOwner(plist_t node = nullptr) : node_{node} {}
  ~PlistOwner() {
    if (node_ != nullptr)
      plist_free(node_);
  }
  PlistOwner(const PlistOwner &) = delete;
  PlistOwner &operator=(const PlistOwner &) = delete;
  [[nodiscard]] plist_t get() const { return node_; }

private:
  plist_t node_{};
};

std::optional<DisplayOrientation> plist_orientation(plist_t root) {
  const auto read_string = [](plist_t node)
      -> std::optional<DisplayOrientation> {
    if (node == nullptr || plist_get_node_type(node) != PLIST_STRING)
      return std::nullopt;
    std::uint64_t length{};
    const auto *value = plist_get_string_ptr(node, &length);
    if (value == nullptr)
      return std::nullopt;
    return parse_orientation(
        std::string_view{value, static_cast<std::size_t>(length)});
  };

  if (const auto value = read_string(
          plist_dict_get_item(root, "UIInterfaceOrientation")))
    return value;
  if (const auto value = read_string(
          plist_dict_get_item(root, "UISupportedInterfaceOrientations")))
    return value;

  const auto array = plist_dict_get_item(root, "UISupportedInterfaceOrientations");
  if (array == nullptr || plist_get_node_type(array) != PLIST_ARRAY)
    return std::nullopt;
  for (std::uint32_t index = 0; index < plist_array_get_size(array); ++index) {
    if (const auto value = read_string(plist_array_get_item(array, index)))
      return value;
  }
  return std::nullopt;
}
#endif

} // namespace

DisplayOrientation detect_application_display_orientation(
    const std::filesystem::path &rootfs, std::string_view executable_path) {
  if (rootfs.empty() || executable_path.empty())
    return DisplayOrientation::Portrait;
  const auto path = info_plist_path(rootfs, executable_path);
  std::ifstream input{path, std::ios::binary};
  if (!input)
    return DisplayOrientation::Portrait;

#if defined(ILEMU_HAS_LIBPLIST)
  const std::string bytes{std::istreambuf_iterator<char>{input},
                          std::istreambuf_iterator<char>{}};
  plist_t parsed = nullptr;
  plist_format_t format = PLIST_FORMAT_NONE;
  if (plist_from_memory(bytes.data(), static_cast<std::uint32_t>(bytes.size()),
                        &parsed, &format) != PLIST_ERR_SUCCESS ||
      parsed == nullptr || plist_get_node_type(parsed) != PLIST_DICT) {
    if (parsed != nullptr)
      plist_free(parsed);
    return DisplayOrientation::Portrait;
  }
  PlistOwner root{parsed};
  return plist_orientation(root.get()).value_or(
      DisplayOrientation::Portrait);
#else
  static_cast<void>(input);
  return DisplayOrientation::Portrait;
#endif
}

const char *display_orientation_name(DisplayOrientation orientation) {
  switch (orientation) {
  case DisplayOrientation::Portrait:
    return "portrait";
  case DisplayOrientation::PortraitUpsideDown:
    return "portrait-upside-down";
  case DisplayOrientation::LandscapeLeft:
    return "landscape-left";
  case DisplayOrientation::LandscapeRight:
    return "landscape-right";
  }
  return "portrait";
}

std::vector<std::uint32_t> orient_display_pixels(
    DisplayGeometry source_geometry, std::span<const std::uint32_t> pixels,
    DisplayOrientation orientation) {
  if (!source_geometry.valid() ||
      pixels.size() != source_geometry.pixel_count())
    return {};
  const auto output_geometry = is_landscape(orientation)
                                   ? DisplayGeometry{source_geometry.height,
                                                     source_geometry.width}
                               : source_geometry;
  std::vector<std::uint32_t> output(output_geometry.pixel_count());
  const auto source_at = [&](std::uint32_t x, std::uint32_t y) {
    return pixels[static_cast<std::size_t>(y) * source_geometry.width + x];
  };
  for (std::uint32_t y = 0; y < output_geometry.height; ++y) {
    for (std::uint32_t x = 0; x < output_geometry.width; ++x) {
      std::uint32_t source_x{};
      std::uint32_t source_y{};
      switch (orientation) {
      case DisplayOrientation::Portrait:
        source_x = x;
        source_y = y;
        break;
      case DisplayOrientation::PortraitUpsideDown:
        source_x = source_geometry.width - 1U - x;
        source_y = source_geometry.height - 1U - y;
        break;
      case DisplayOrientation::LandscapeLeft:
        source_x = y;
        source_y = x;
        break;
      case DisplayOrientation::LandscapeRight:
        source_x = source_geometry.width - 1U - y;
        source_y = source_geometry.height - 1U - x;
        break;
      }
      output[static_cast<std::size_t>(y) * output_geometry.width + x] =
          source_at(source_x, source_y);
    }
  }
  return output;
}

} // namespace ilemu
