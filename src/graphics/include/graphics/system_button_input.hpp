#pragma once

namespace ilemu {

// Physical iPhone controls as exposed through the iPhone OS 1.0 GSEvent ABI.
// "Home" is named "Menu" by the firmware but uses the user-facing name here.
enum class SystemButton {
    Home,
    Lock,
    VolumeUp,
    VolumeDown,
};

enum class SystemButtonPhase {
    Down,
    Up,
};

struct SystemButtonInput {
    SystemButton button { SystemButton::Home };
    SystemButtonPhase phase { SystemButtonPhase::Down };
};

} // namespace ilemu
