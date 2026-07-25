#pragma once

#include <functional>
#include <memory>

namespace ilemu {

class UserlandHleRegistry;
class WifiState;
struct WifiSnapshot;

using WifiStateProvider = std::function<std::shared_ptr<WifiState>()>;

// Supplies the high-level telephony state needed by SpringBoard without
// emulating a baseband device or the CommCenter transport protocol.
void register_core_telephony_hle(UserlandHleRegistry& registry);
void register_core_telephony_hle(
    UserlandHleRegistry& registry, WifiStateProvider wifi_state,
    std::function<void(const WifiSnapshot&, const WifiSnapshot&)>
        wifi_state_changed = {});

}  // namespace ilemu
