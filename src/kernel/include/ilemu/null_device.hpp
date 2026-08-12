#pragma once

#include <array>
#include <string_view>

namespace ilemu::bsd::null_device {

inline constexpr std::string_view descriptor_kind{"null"};
inline constexpr unsigned device_minor = 5;
inline constexpr std::array<std::string_view, 2> paths{
    "/dev/null", "/dev/autofs_nowait"};
inline constexpr std::array<std::string_view, 2> directory_names{
    "null", "autofs_nowait"};

[[nodiscard]] inline bool is_path(std::string_view candidate) {
  for (const auto path : paths) {
    if (candidate == path) {
      return true;
    }
  }
  return false;
}

} // namespace ilemu::bsd::null_device
