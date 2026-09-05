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
    configure_gles_accelerated_factory(nullptr);
#endif
}

} // namespace ilemu
