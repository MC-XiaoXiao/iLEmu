// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Translate SDL window, keyboard and pointer events into emulator
// input.

#include "sdl_input.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "graphics/display.hpp"

#if defined(ILEMU_HAS_SDL2)
#include <SDL.h>
#endif

namespace ilemu {
namespace {

#if defined(ILEMU_HAS_SDL2)
    DisplayGeometry input_coordinate_geometry(SDL_Window* window)
    {
        int width { };
        int height { };
        if (auto* renderer = SDL_GetRenderer(window))
            SDL_RenderGetLogicalSize(renderer, &width, &height);
        // SDL renderer events use its logical coordinates; a native Vulkan
        // window without an SDL renderer delivers window coordinates.
        if (width <= 0 || height <= 0)
            SDL_GetWindowSize(window, &width, &height);
        return { static_cast<std::uint32_t>(std::max(width, 0)),
            static_cast<std::uint32_t>(std::max(height, 0)) };
    }

    struct MappedTouch {
        TouchInput input;
        bool inside { };
    };

    MappedTouch map_window_touch(float x, float y, TouchPhase phase,
        DisplayViewport viewport, DisplayGeometry geometry,
        DisplayOrientation orientation)
    {
        const auto viewport_width = std::max(viewport.width, 1U);
        const auto viewport_height = std::max(viewport.height, 1U);
        const auto normalized_x =
            std::clamp((x - static_cast<float>(viewport.x)) /
                           static_cast<float>(viewport_width),
                0.0F, 1.0F);
        const auto normalized_y =
            std::clamp((y - static_cast<float>(viewport.y)) /
                           static_cast<float>(viewport_height),
                0.0F, 1.0F);
        const auto guest_width =
            static_cast<float>(geometry.valid() ? geometry.width : 0U);
        const auto guest_height =
            static_cast<float>(geometry.valid() ? geometry.height : 0U);
        float guest_x = normalized_x * guest_width;
        float guest_y = normalized_y * guest_height;
        switch (orientation) {
        case DisplayOrientation::Portrait:
            break;
        case DisplayOrientation::PortraitUpsideDown:
            guest_x = (1.0F - normalized_x) * guest_width;
            guest_y = (1.0F - normalized_y) * guest_height;
            break;
        case DisplayOrientation::LandscapeLeft:
            guest_x = normalized_y * guest_width;
            guest_y = (1.0F - normalized_x) * guest_height;
            break;
        case DisplayOrientation::LandscapeRight:
            guest_x = (1.0F - normalized_y) * guest_width;
            guest_y = normalized_x * guest_height;
            break;
        }
        const auto viewport_right =
            static_cast<float>(viewport.x) + static_cast<float>(viewport.width);
        const auto viewport_bottom = static_cast<float>(viewport.y) +
                                     static_cast<float>(viewport.height);
        const auto inside =
            x >= static_cast<float>(viewport.x) && x < viewport_right &&
            y >= static_cast<float>(viewport.y) && y < viewport_bottom;
        // Touch positions are continuous. Preserve the display scale across
        // the whole viewport and keep a clamped edge inside the guest bounds.
        guest_x = std::min(guest_x, std::nextafter(guest_width, 0.0F));
        guest_y = std::min(guest_y, std::nextafter(guest_height, 0.0F));
        return MappedTouch { TouchInput { phase, guest_x, guest_y }, inside };
    }

    MappedTouch map_finger(TouchPhase phase, float x, float y, int window_width,
        int window_height, DisplayViewport viewport, DisplayGeometry geometry,
        DisplayOrientation orientation)
    {
        return map_window_touch(
            std::clamp(x, 0.0F, 1.0F) *
                static_cast<float>(std::max(window_width, 1)),
            std::clamp(y, 0.0F, 1.0F) *
                static_cast<float>(std::max(window_height, 1)),
            phase, viewport, geometry, orientation);
    }

    std::optional<SystemButton> map_key(SDL_Keycode key)
    {
        switch (key) {
        case SDLK_h:
            return SystemButton::Home;
        case SDLK_l:
            return SystemButton::Lock;
        case SDLK_EQUALS:
        case SDLK_PLUS:
        case SDLK_KP_PLUS:
            return SystemButton::VolumeUp;
        case SDLK_MINUS:
        case SDLK_KP_MINUS:
            return SystemButton::VolumeDown;
        default:
            return std::nullopt;
        }
    }
#endif

} // namespace

bool SdlInput::poll(SDL_Window* window)
{
#if defined(ILEMU_HAS_SDL2)
    if (!SDL_IsTextInputActive())
        SDL_StartTextInput();
    SDL_Event event { };
    while (SDL_PollEvent(&event) != 0) {
        const auto coordinates = input_coordinate_geometry(window);
        process_event(event, static_cast<int>(coordinates.width),
            static_cast<int>(coordinates.height),
            fit_display_viewport(display_geometry_, coordinates));
    }
#else
    static_cast<void>(window);
#endif
    return running_;
}

bool SdlInput::wait(SDL_Window* window, std::chrono::nanoseconds timeout)
{
#if defined(ILEMU_HAS_SDL2)
    if (!running_)
        return false;
    int timeout_milliseconds = 0;
    const auto indefinite = timeout == std::chrono::nanoseconds::max();
    if (!indefinite) {
        if (timeout <= std::chrono::nanoseconds::zero())
            return poll(window);
        const auto rounded =
            std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
        auto milliseconds = rounded.count();
        if (std::chrono::duration_cast<std::chrono::nanoseconds>(rounded) <
            timeout) {
            ++milliseconds;
        }
        milliseconds = std::min<std::int64_t>(
            milliseconds, std::numeric_limits<int>::max());
        timeout_milliseconds = static_cast<int>(milliseconds);
    }

    SDL_Event event { };
    const auto received =
        indefinite ? SDL_WaitEvent(&event)
                   : SDL_WaitEventTimeout(&event, timeout_milliseconds);
    if (received != 0) {
        const auto coordinates = input_coordinate_geometry(window);
        process_event(event, static_cast<int>(coordinates.width),
            static_cast<int>(coordinates.height),
            fit_display_viewport(display_geometry_, coordinates));
    }
#else
    static_cast<void>(window);
    static_cast<void>(timeout);
#endif
    return running_;
}

void SdlInput::process_event(const SDL_Event& event, int window_width,
    int window_height, DisplayViewport viewport)
{
#if defined(ILEMU_HAS_SDL2)
    const auto queue_touch = [this, &event](TouchInput input) {
        touch_events_.push_back({ input, event.common.timestamp });
    };
    switch (event.type) {
    case SDL_WINDOWEVENT:
        switch (event.window.event) {
        case SDL_WINDOWEVENT_EXPOSED:
        case SDL_WINDOWEVENT_SHOWN:
        case SDL_WINDOWEVENT_RESTORED:
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            redraw_requested_ = true;
            break;
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_RESIZED:
            surface_change_requested_ = true;
            redraw_requested_ = true;
            break;
        default:
            break;
        }
        break;
    case SDL_QUIT:
        running_ = false;
        break;
    case SDL_TEXTINPUT:
        text_events_.push_back(TextInput { event.text.text });
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        const auto key = event.key.keysym.sym;
        const auto down = event.type == SDL_KEYDOWN;
        const auto control = (event.key.keysym.mod & KMOD_CTRL) != 0;
        // Remember the Down binding so releasing Ctrl first still releases
        // the same guest physical button on KeyUp.
        if ((down && control) || (!down && shortcut_keys_.contains(key))) {
            if (down && event.key.repeat == 0)
                shortcut_keys_.insert(key);
            if (!down)
                shortcut_keys_.erase(key);
            if (event.key.repeat == 0) {
                if (down && key == SDLK_r)
                    ringer_switch_events_.push_back(RingerSwitchInput { });
                else if (const auto button = map_key(key))
                    button_events_.push_back(SystemButtonInput {
                        *button, down ? SystemButtonPhase::Down
                                      : SystemButtonPhase::Up });
            }
        } else if (down && (event.key.keysym.mod &
                               (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) == 0) {
            if (key == SDLK_BACKSPACE)
                text_events_.push_back(TextInput { "\b" });
            else if (key == SDLK_RETURN || key == SDLK_KP_ENTER)
                text_events_.push_back(TextInput { "\n" });
            else if (key == SDLK_TAB)
                text_events_.push_back(TextInput { "\t" });
        }
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (event.button.button == SDL_BUTTON_LEFT &&
            event.button.which != SDL_TOUCH_MOUSEID) {
            const auto mapped =
                map_window_touch(static_cast<float>(event.button.x),
                    static_cast<float>(event.button.y), TouchPhase::Down,
                    viewport, geometry_, orientation_);
            if (mapped.inside) {
                mouse_active_ = true;
                queue_touch(mapped.input);
            }
        }
        break;
    case SDL_MOUSEMOTION:
        if (mouse_active_ && event.motion.which != SDL_TOUCH_MOUSEID) {
            queue_touch(
                map_window_touch(static_cast<float>(event.motion.x),
                    static_cast<float>(event.motion.y), TouchPhase::Move,
                    viewport, geometry_, orientation_)
                    .input);
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (event.button.button == SDL_BUTTON_LEFT && mouse_active_ &&
            event.button.which != SDL_TOUCH_MOUSEID) {
            queue_touch(
                map_window_touch(static_cast<float>(event.button.x),
                    static_cast<float>(event.button.y), TouchPhase::Up,
                    viewport, geometry_, orientation_)
                    .input);
            mouse_active_ = false;
        }
        break;
    case SDL_FINGERDOWN: {
        const auto mapped =
            map_finger(TouchPhase::Down, event.tfinger.x, event.tfinger.y,
                window_width, window_height, viewport, geometry_, orientation_);
        if (mapped.inside) {
            active_fingers_.insert(
                static_cast<std::int64_t>(event.tfinger.fingerId));
            queue_touch(mapped.input);
        }
        break;
    }
    case SDL_FINGERMOTION:
        if (active_fingers_.contains(
                static_cast<std::int64_t>(event.tfinger.fingerId))) {
            queue_touch(map_finger(TouchPhase::Move,
                event.tfinger.x, event.tfinger.y, window_width, window_height,
                viewport, geometry_, orientation_)
                    .input);
        }
        break;
    case SDL_FINGERUP:
        if (active_fingers_.erase(
                static_cast<std::int64_t>(event.tfinger.fingerId)) != 0U) {
            queue_touch(map_finger(TouchPhase::Up, event.tfinger.x,
                event.tfinger.y, window_width, window_height, viewport,
                geometry_, orientation_)
                    .input);
        }
        break;
    default:
        break;
    }
#else
    static_cast<void>(event);
    static_cast<void>(window_width);
    static_cast<void>(window_height);
    static_cast<void>(viewport);
#endif
}

std::vector<TextInput> SdlInput::take_text_events()
{
    auto events = std::move(text_events_);
    text_events_.clear();
    return events;
}

std::vector<TouchInput> SdlInput::take_touch_events()
{
    std::vector<TouchInput> events;
#if defined(ILEMU_HAS_SDL2)
    if (touch_events_.empty())
        return events;

    // Use one clock reference for the whole batch so queued SDL events keep
    // their sample intervals even after a slow guest iteration.
    const auto sdl_now = SDL_GetTicks();
    const auto host_now = std::chrono::steady_clock::now();
    events.reserve(touch_events_.size());
    for (auto& pending : touch_events_) {
        // SDL event ticks are 32 bits. Unsigned subtraction recovers the age
        // across rollover as long as the sample is within the last half-cycle.
        const auto age =
            static_cast<std::uint32_t>(sdl_now - pending.sdl_timestamp);
        if (age <= static_cast<std::uint32_t>(
                       std::numeric_limits<std::int32_t>::max())) {
            pending.input.host_sample_time =
                host_now - std::chrono::milliseconds { age };
        }
        events.push_back(pending.input);
    }
    touch_events_.clear();
#endif
    return events;
}

std::vector<SystemButtonInput> SdlInput::take_button_events()
{
    auto events = std::move(button_events_);
    button_events_.clear();
    return events;
}

std::vector<RingerSwitchInput> SdlInput::take_ringer_switch_events()
{
    auto events = std::move(ringer_switch_events_);
    ringer_switch_events_.clear();
    return events;
}

} // namespace ilemu
