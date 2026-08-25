#pragma once

#include <cstdint>

namespace ilemu {

enum class TouchPhase : std::uint8_t {
    Down,
    Move,
    Up,
    Cancel,
};

struct TouchInput {
    TouchPhase phase { TouchPhase::Down };
    float x { };
    float y { };
};

} // namespace ilemu
