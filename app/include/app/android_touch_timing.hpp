// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <SDL.h>

#include <cstdint>

namespace ilemu::android_touch_timing {

bool begin_live_batch();
void annotate(SDL_TouchID device, SDL_FingerID finger, Uint32 type,
    std::int64_t source_ns, float x, float y);
void finish();

class EventWatch {
public:
    EventWatch() = default;
    EventWatch(const EventWatch&) = delete;
    EventWatch& operator=(const EventWatch&) = delete;
    ~EventWatch();

    void start();
    void stop();

private:
    bool active_ { };
};

} // namespace ilemu::android_touch_timing
