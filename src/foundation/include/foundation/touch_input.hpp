// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Describe guest touch phases and coordinates independently of host
// input APIs.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

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
    // Host sample time, when supplied by the input source.
    std::optional<std::chrono::steady_clock::time_point> host_sample_time { };
};

} // namespace ilemu
