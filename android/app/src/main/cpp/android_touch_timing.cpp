// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "app/android_touch_timing.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>

namespace ilemu::android_touch_timing {
namespace {

struct ClockAnchor {
    std::int64_t host_ns;
    Uint64 sdl_ticks;

    [[nodiscard]] std::optional<Uint32> timestamp_at(std::int64_t source_ns) const
    {
        if (source_ns <= 0 || source_ns > host_ns)
            return std::nullopt;
        const auto age_ms = static_cast<Uint64>((host_ns - source_ns) / 1'000'000);
        if (age_ms > sdl_ticks ||
            age_ms > static_cast<Uint64>(std::numeric_limits<Sint32>::max()))
            return std::nullopt;
        return static_cast<Uint32>(sdl_ticks - age_ms);
    }
};

struct AndroidSample {
    SDL_TouchID device;
    SDL_FingerID finger;
    Uint32 expected_type;
    std::optional<Uint32> timestamp;
    float x;
    float y;
};

// The JNI and SDL display targets may lower TLS differently. Keep every access
// to this producer state in this translation unit.
thread_local std::optional<ClockAnchor> anchor;
thread_local std::optional<AndroidSample> sample;
std::atomic<bool> installed { false };

int SDLCALL watch(void*, SDL_Event* event)
{
    // Pinned SDL 2.32.10 calls event watches after stamping, synchronously
    // on the pushing thread, and copies the resulting event into its queue.
    if (event->type != SDL_FINGERDOWN && event->type != SDL_FINGERMOTION &&
        event->type != SDL_FINGERUP)
        return 0;
    if (sample && event->type == sample->expected_type &&
        event->tfinger.touchId == sample->device &&
        event->tfinger.fingerId == sample->finger &&
        (event->type == SDL_FINGERUP ||
            (event->tfinger.x == sample->x && event->tfinger.y == sample->y))) {
        if (sample->timestamp)
            event->tfinger.timestamp = *sample->timestamp;
        if (event->type == SDL_FINGERUP) {
            event->tfinger.x = sample->x;
            event->tfinger.y = sample->y;
        }
        sample.reset();
    }
    return 0;
}

} // namespace

void finish()
{
    sample.reset();
    anchor.reset();
}

bool begin_live_batch()
{
    if (!installed.load(std::memory_order_acquire) || anchor)
        return false;
    // Android MotionEvent uses the monotonic uptime clock. Pair that clock
    // with SDL's ticks for each batch to avoid drift across the two timers.
    using Clock = std::chrono::steady_clock;
    const auto before = Clock::now();
    const auto sdl_ticks = SDL_GetTicks64();
    const auto after = Clock::now();
    if (after - before > std::chrono::milliseconds { 1 })
        return false;
    const auto host_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        (before + (after - before) / 2).time_since_epoch()).count();
    sample.reset();
    anchor = ClockAnchor { host_ns, sdl_ticks };
    return true;
}

void annotate(SDL_TouchID device, SDL_FingerID finger, Uint32 type,
    std::int64_t source_ns, float x, float y)
{
    sample.reset();
    if (!anchor || device == SDL_MOUSE_TOUCHID ||
        (type != SDL_FINGERDOWN && type != SDL_FINGERMOTION && type != SDL_FINGERUP))
        return;
    const auto timestamp = anchor->timestamp_at(source_ns);
    if ((timestamp || type == SDL_FINGERUP) && std::isfinite(x) && std::isfinite(y))
        sample = AndroidSample { device, finger, type, timestamp, x, y };
}

EventWatch::~EventWatch()
{
    stop();
}

void EventWatch::start()
{
    if (active_ || SDL_GetEventFilter(nullptr, nullptr))
        return;
    SDL_AddEventWatch(watch, nullptr);
    active_ = true;
    installed.store(true, std::memory_order_release);
}

void EventWatch::stop()
{
    if (!active_)
        return;
    installed.store(false, std::memory_order_release);
    SDL_DelEventWatch(watch, nullptr);
    active_ = false;
}

} // namespace ilemu::android_touch_timing
