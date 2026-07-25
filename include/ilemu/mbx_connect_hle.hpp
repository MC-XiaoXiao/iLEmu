#pragma once

namespace ilemu {

class UserlandHleRegistry;

// Registers the MBXConnect control calls that LayerKit issues directly during
// startup. Rendering and surface transport stay in the MBX2D/CoreSurface HLEs;
// no PowerVR connection, command buffer, or register interface is exposed.
void register_mbx_connect_hle(UserlandHleRegistry& registry);

}  // namespace ilemu
