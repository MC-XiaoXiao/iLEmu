#pragma once

namespace ilemu {

// Install native backend support before configuring or creating a renderer.
// The emulator's renderer contract and software fallback need no native SDK.
void register_native_gles_renderer();

} // namespace ilemu
