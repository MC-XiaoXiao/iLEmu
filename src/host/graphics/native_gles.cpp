#include "host/native_gles.hpp"

#include "graphics/gles_renderer.hpp"

#if defined(ILEMU_HAS_VULKAN)
#include "vulkan_gles_renderer.hpp"
#endif

namespace ilemu {

void register_native_gles_renderer()
{
#if defined(ILEMU_HAS_VULKAN)
    configure_gles_accelerated_factory(create_vulkan_gles_renderer);
#else
    configure_gles_accelerated_factory(
        [](const std::filesystem::path&, const VulkanPresenterConfiguration*,
            GlesDeviceSelection, std::string* failure) noexcept
            -> std::unique_ptr<GlesRenderer> {
            if (failure != nullptr)
                *failure = "Vulkan support was not built";
            return { };
        });
#endif
}

} // namespace ilemu
