#pragma once

namespace ilemu {

class UserlandHleRegistry;

// Reports the absence of a controller at MobileBluetooth's public session
// boundary so guest clients can take their native offline path.
void register_bluetooth_manager_hle(UserlandHleRegistry& registry);

} // namespace ilemu
