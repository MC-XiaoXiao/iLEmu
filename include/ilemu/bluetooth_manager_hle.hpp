#pragma once

namespace ilemu {

class UserlandHleRegistry;

// Keeps SpringBoard's BluetoothManager in its supported no-session state when
// no guest Bluetooth device is exposed.
void register_bluetooth_manager_hle(UserlandHleRegistry& registry);

}  // namespace ilemu
