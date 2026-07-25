#pragma once

#include <memory>
#include <string>

#include "ilemu/gles_renderer.hpp"

namespace ilemu {

// Returns null when Vulkan is unavailable or only exposes a CPU device.
// Failure details let the policy layer reject an explicitly requested backend.
[[nodiscard]] std::unique_ptr<GlesRenderer>
create_vulkan_gles_renderer(std::string* failure = nullptr) noexcept;

} // namespace ilemu
