// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Register and select the host accelerated GLES renderer.

#include "host/native_gles.hpp"

#include "graphics/gles_renderer.hpp"
#if defined(ILEMU_HAS_HOST_GLES)
#include "egl_program_renderer.hpp"
#endif

#if defined(ILEMU_HAS_VULKAN)
#include "vulkan_gles_renderer.hpp"
#endif

namespace ilemu {

void register_native_gles_renderer()
{
#if defined(ILEMU_HAS_HOST_GLES)
    configure_gles_program_renderer_factory(create_egl_program_renderer);
#endif
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
