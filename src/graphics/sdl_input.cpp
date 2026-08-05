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
                     DisplayGeometry geometry,
                     DisplayOrientation orientation) {
  int width = 1;
  int height = 1;
  SDL_GetWindowSize(window, &width, &height);
  const auto normalized_x = std::clamp(
      static_cast<float>(x) / static_cast<float>(std::max(width, 1)), 0.0F,
      1.0F);
  const auto normalized_y = std::clamp(
      static_cast<float>(y) / static_cast<float>(std::max(height, 1)), 0.0F,
      1.0F);
  float guest_x = normalized_x * static_cast<float>(geometry.width - 1U);
  float guest_y = normalized_y * static_cast<float>(geometry.height - 1U);
  switch (orientation) {
  case DisplayOrientation::Portrait:
    break;
  case DisplayOrientation::PortraitUpsideDown:
    guest_x = (1.0F - normalized_x) * static_cast<float>(geometry.width - 1U);
    guest_y = (1.0F - normalized_y) * static_cast<float>(geometry.height - 1U);
    break;
  case DisplayOrientation::LandscapeLeft:
    guest_x = normalized_y * static_cast<float>(geometry.width - 1U);
    guest_y = (1.0F - normalized_x) * static_cast<float>(geometry.height - 1U);
    break;
  case DisplayOrientation::LandscapeRight:
    guest_x = (1.0F - normalized_y) * static_cast<float>(geometry.width - 1U);
    guest_y = normalized_x * static_cast<float>(geometry.height - 1U);
    break;
  }
  return TouchInput{phase, guest_x, guest_y};
}

TouchInput map_finger(TouchPhase phase, float x, float y,
                      DisplayGeometry geometry,
                      DisplayOrientation orientation) {
  const auto normalized_x = std::clamp(x, 0.0F, 1.0F);
  const auto normalized_y = std::clamp(y, 0.0F, 1.0F);
  float guest_x = normalized_x * static_cast<float>(geometry.width - 1U);
  float guest_y = normalized_y * static_cast<float>(geometry.height - 1U);
  switch (orientation) {
  case DisplayOrientation::Portrait:
    break;
  case DisplayOrientation::PortraitUpsideDown:
    guest_x = (1.0F - normalized_x) * static_cast<float>(geometry.width - 1U);
    guest_y = (1.0F - normalized_y) * static_cast<float>(geometry.height - 1U);
    break;
  case DisplayOrientation::LandscapeLeft:
    guest_x = normalized_y * static_cast<float>(geometry.width - 1U);
    guest_y = (1.0F - normalized_x) * static_cast<float>(geometry.height - 1U);
    break;
  case DisplayOrientation::LandscapeRight:
    guest_x = (1.0F - normalized_y) * static_cast<float>(geometry.width - 1U);
    guest_y = normalized_x * static_cast<float>(geometry.height - 1U);
    break;
  }
  return TouchInput{phase, guest_x, guest_y};
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
                                          geometry_, orientation_));
      }
      break;
    case SDL_MOUSEMOTION:
      if (mouse_active_ && event.motion.which != SDL_TOUCH_MOUSEID) {
        touch_events_.push_back(map_mouse(window, TouchPhase::Move,
                                          event.motion.x, event.motion.y,
                                          geometry_, orientation_));
      }
      break;
    case SDL_MOUSEBUTTONUP:
      if (event.button.button == SDL_BUTTON_LEFT && mouse_active_ &&
          event.button.which != SDL_TOUCH_MOUSEID) {
        touch_events_.push_back(
            map_mouse(window, TouchPhase::Up, event.button.x, event.button.y,
                      geometry_, orientation_));
        mouse_active_ = false;
      }
      break;
    case SDL_FINGERDOWN:
      touch_events_.push_back(
          map_finger(TouchPhase::Down, event.tfinger.x, event.tfinger.y,
                     geometry_, orientation_));
      break;
    case SDL_FINGERMOTION:
      touch_events_.push_back(
          map_finger(TouchPhase::Move, event.tfinger.x, event.tfinger.y,
                     geometry_, orientation_));
      break;
    case SDL_FINGERUP:
      touch_events_.push_back(
          map_finger(TouchPhase::Up, event.tfinger.x, event.tfinger.y,
                     geometry_, orientation_));
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
