#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace ilemu {

// Darwin clients observe the physical ringer/silent switch through this
// notification key. The device model owns the value; audio policy remains in
// the guest firmware.
inline constexpr std::string_view ringer_switch_notification_name {
    "com.apple.system.ringerstate"
};
// iPhone OS 2.x CoreMedia uses SpringBoard's historical notify key. Both
// names describe the same physical switch; keeping the aliases here lets the
// generic device model serve older and newer firmware contracts alike.
inline constexpr std::string_view springboard_ringer_switch_notification_name {
    "com.apple.springboard.ringerstate"
};

class RingerSwitchState {
public:
    [[nodiscard]] bool active() const noexcept
    {
        return active_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool set_active(bool active) noexcept
    {
        return active_.exchange(active, std::memory_order_relaxed) != active;
    }

    [[nodiscard]] bool toggle() noexcept
    {
        auto active = active_.load(std::memory_order_relaxed);
        while (!active_.compare_exchange_weak(active, !active,
            std::memory_order_relaxed, std::memory_order_relaxed)) { }
        return !active;
    }

private:
    // A physical iPhone boots with one of two stable switch positions. Ringing
    // is the least surprising default for a simulator without saved hardware
    // state and lets the firmware apply its ordinary system-sound policy.
    std::atomic_bool active_ { true };
};

// A host key represents movement of the physical two-position switch, not an
// independently cached target state. The device model is the sole state owner.
struct RingerSwitchInput { };

} // namespace ilemu
