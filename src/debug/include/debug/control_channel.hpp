#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "foundation/device_model.hpp"
#include "foundation/display_geometry.hpp"
#include "foundation/system_button_input.hpp"
#include "foundation/touch_input.hpp"

namespace ilemu {

enum class LiveControlCommandKind {
    Touch,
    Gesture,
    Button,
    ButtonHold,
    Home,
    Lock,
    VolumeUp,
    VolumeDown,
    RingerRing,
    RingerSilent,
    Snapshot,
    SnapshotSequence,
    PerfBegin,
    PerfEnd,
    Status,
    Processes,
    Threads,
    Help,
    Quit,
    Error,
};

struct LiveTouchEvent {
    std::chrono::milliseconds delay { };
    TouchInput input;
};

struct LiveControlCommand {
    LiveControlCommandKind kind { LiveControlCommandKind::Error };
    TouchInput touch;
    std::vector<LiveTouchEvent> gesture;
    SystemButtonInput system_button;
    std::chrono::milliseconds button_hold { };
    bool wake_display { };
    bool home_wake_barrier { };
    std::filesystem::path path;
    std::chrono::milliseconds snapshot_interval { };
    std::size_t snapshot_count { };
    std::string message;
};

class ControlChannel {
public:
    virtual ~ControlChannel() = default;
    [[nodiscard]] virtual std::vector<LiveControlCommand> poll() = 0;
    virtual void wait_for(std::chrono::nanoseconds timeout) = 0;
    [[nodiscard]] virtual bool closed() const = 0;
};

} // namespace ilemu
