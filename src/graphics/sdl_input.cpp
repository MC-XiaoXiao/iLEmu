#include "sdl_input.hpp"

#include <algorithm>
#include <optional>

#include "ilemu/display.hpp"

#if defined(ILEMU_HAS_SDL2)
#include <SDL.h>
#endif

namespace ilemu {
namespace {

#if defined(ILEMU_HAS_SDL2)
TouchInput map_mouse(SDL_Window *window, TouchPhase phase, int x, int y,
                     DisplayGeometry geometry) {
  int width = 1;
  int height = 1;
  SDL_GetWindowSize(window, &width, &height);
  const auto scale = [](int value, int extent, std::uint32_t guest_extent) {
    const auto normalized = std::clamp(
        static_cast<float>(value) / static_cast<float>(std::max(extent, 1)),
        0.0F, 1.0F);
    return normalized * static_cast<float>(guest_extent - 1U);
  };
  return TouchInput{phase, scale(x, width, geometry.width),
                    scale(y, height, geometry.height)};
}

TouchInput map_finger(TouchPhase phase, float x, float y,
                      DisplayGeometry geometry) {
  return TouchInput{phase,
                    std::clamp(x, 0.0F, 1.0F) *
                        static_cast<float>(geometry.width - 1U),
                    std::clamp(y, 0.0F, 1.0F) *
                        static_cast<float>(geometry.height - 1U)};
}

std::optional<SystemButton> map_key(SDL_Keycode key) {
  switch (key) {
  case SDLK_HOME:
    return SystemButton::Home;
  case SDLK_END:
    return SystemButton::Lock;
  case SDLK_PAGEUP:
    return SystemButton::VolumeUp;
  case SDLK_PAGEDOWN:
    return SystemButton::VolumeDown;
  default:
    return std::nullopt;
  }
}
#endif

} // namespace

bool SdlInput::poll(SDL_Window *window) {
#if defined(ILEMU_HAS_SDL2)
  SDL_Event event{};
  while (SDL_PollEvent(&event) != 0) {
    switch (event.type) {
    case SDL_QUIT:
      running_ = false;
      break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
      if (event.key.repeat == 0) {
        if (event.type == SDL_KEYDOWN &&
            event.key.keysym.sym == SDLK_DELETE) {
          ringer_switch_events_.push_back(RingerSwitchInput{});
          break;
        }
        if (const auto button = map_key(event.key.keysym.sym)) {
          button_events_.push_back(SystemButtonInput{
              *button, event.type == SDL_KEYDOWN ? SystemButtonPhase::Down
                                                 : SystemButtonPhase::Up});
        }
      }
      break;
    case SDL_MOUSEBUTTONDOWN:
      if (event.button.button == SDL_BUTTON_LEFT &&
          event.button.which != SDL_TOUCH_MOUSEID) {
        mouse_active_ = true;
        touch_events_.push_back(map_mouse(window, TouchPhase::Down,
                                          event.button.x, event.button.y,
                                          geometry_));
      }
      break;
    case SDL_MOUSEMOTION:
      if (mouse_active_ && event.motion.which != SDL_TOUCH_MOUSEID) {
        touch_events_.push_back(map_mouse(window, TouchPhase::Move,
                                          event.motion.x, event.motion.y,
                                          geometry_));
      }
      break;
    case SDL_MOUSEBUTTONUP:
      if (event.button.button == SDL_BUTTON_LEFT && mouse_active_ &&
          event.button.which != SDL_TOUCH_MOUSEID) {
        touch_events_.push_back(
            map_mouse(window, TouchPhase::Up, event.button.x, event.button.y,
                      geometry_));
        mouse_active_ = false;
      }
      break;
    case SDL_FINGERDOWN:
      touch_events_.push_back(
          map_finger(TouchPhase::Down, event.tfinger.x, event.tfinger.y,
                     geometry_));
      break;
    case SDL_FINGERMOTION:
      touch_events_.push_back(
          map_finger(TouchPhase::Move, event.tfinger.x, event.tfinger.y,
                     geometry_));
      break;
    case SDL_FINGERUP:
      touch_events_.push_back(
          map_finger(TouchPhase::Up, event.tfinger.x, event.tfinger.y,
                     geometry_));
      break;
    default:
      break;
    }
  }
#else
  static_cast<void>(window);
#endif
  return running_;
}

std::vector<TouchInput> SdlInput::take_touch_events() {
  auto events = std::move(touch_events_);
  touch_events_.clear();
  return events;
}

std::vector<SystemButtonInput> SdlInput::take_button_events() {
  auto events = std::move(button_events_);
  button_events_.clear();
  return events;
}

std::vector<RingerSwitchInput> SdlInput::take_ringer_switch_events() {
  auto events = std::move(ringer_switch_events_);
  ringer_switch_events_.clear();
  return events;
}

} // namespace ilemu
