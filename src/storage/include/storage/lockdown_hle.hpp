#pragma once

#include <optional>

#include "device_state/lockdown_profile.hpp"

namespace ilemu {

class UserlandHleRegistry;

// Exposes an explicitly configured simulator activation state through the
// firmware's public liblockdown client boundary. Preserve mode leaves every
// request with the stock daemon.
void register_lockdown_hle(UserlandHleRegistry& registry,
    std::optional<bool> activated, LockdownFirmwareProfile profile);

} // namespace ilemu
