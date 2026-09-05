#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "debug/control_channel.hpp"
#include "foundation/device_profile.hpp"
#include "graphics/display_geometry.hpp"
#include "graphics/system_button_input.hpp"
#include "graphics/touch_input.hpp"

namespace ilemu {

// Non-blocking line-oriented control channel used by headless interactive
// sessions. The descriptor remains owned by the caller.
class LiveControl final : public ControlChannel {
public:
    explicit LiveControl(int descriptor,
        DisplayGeometry geometry = default_display_geometry,
        SystemGestureProfile system_gestures = classic_compact_system_gestures);

    [[nodiscard]] std::vector<LiveControlCommand> poll() override;
    // Blocks until the descriptor is readable/hung up or the timeout expires.
    // The next poll() still owns buffering and command parsing.
    void wait_for(std::chrono::nanoseconds timeout) override;
    [[nodiscard]] bool closed() const override { return closed_; }

private:
    [[nodiscard]] std::vector<LiveControlCommand> parse_line(std::string line);

    int descriptor_ { };
    DisplayGeometry geometry_;
    SystemGestureProfile system_gestures_;
    std::string buffered_input_;
    bool closed_ { };
};

} // namespace ilemu
