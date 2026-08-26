#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

#include "ilemu/device_profile.hpp"

namespace ilemu {

inline constexpr std::string_view graphics_services_capability_object_name {
    "GSCapabilities"
};

// Build the Darwin GraphicsServices shared-memory payload from the firmware's
// SpringBoard capability plist and the selected device profile. The returned
// bytes begin with the 32-bit XML length expected by GSCopyCapabilities.
[[nodiscard]] std::vector<std::byte>
make_graphics_services_capability_memory(
    const std::filesystem::path& rootfs, const DeviceProfile& profile);

} // namespace ilemu
