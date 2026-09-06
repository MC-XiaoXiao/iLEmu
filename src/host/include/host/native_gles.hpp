#pragma once

namespace ilemu {

// Install native backend support before configuring or creating a renderer.
// Hardware and CPU devices use the same native renderer. A host built without
// Vulkan reports its missing dependency instead of changing renderers.
void register_native_gles_renderer();

} // namespace ilemu
