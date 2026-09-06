#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "graphics/gles_renderer.hpp"

namespace ilemu {

// Returns null when Vulkan exposes no device matching the selection policy.
// Failure details let the policy layer reject an explicitly requested backend.
[[nodiscard]] std::unique_ptr<GlesRenderer> create_vulkan_gles_renderer(
    const std::filesystem::path& pipeline_cache,
    const VulkanPresenterConfiguration* presenter,
    GlesDeviceSelection selection,
    std::string* failure = nullptr) noexcept;

} // namespace ilemu
