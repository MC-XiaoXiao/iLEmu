#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "ilemu/gles_renderer.hpp"

namespace ilemu {

// Returns null when Vulkan is unavailable or only exposes a CPU device.
// Failure details let the policy layer reject an explicitly requested backend.
[[nodiscard]] std::unique_ptr<GlesRenderer>
create_vulkan_gles_renderer(const std::filesystem::path& pipeline_cache,
                            const VulkanPresenterConfiguration* presenter,
                            std::string* failure = nullptr) noexcept;

} // namespace ilemu
