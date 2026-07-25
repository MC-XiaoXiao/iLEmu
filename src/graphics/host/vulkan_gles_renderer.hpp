#pragma once

#include <memory>

#include "ilegacysim/gles_renderer.hpp"

namespace ilegacysim {

// Returns null when Vulkan is unavailable or only exposes a CPU device. The
// caller keeps the portable software renderer as the unconditional fallback.
[[nodiscard]] std::unique_ptr<GlesRenderer>
create_vulkan_gles_renderer() noexcept;

} // namespace ilegacysim
