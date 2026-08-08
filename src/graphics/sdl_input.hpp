#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "ilemu/display_geometry.hpp"
#include "ilemu/ringer_switch_state.hpp"
#include "ilemu/system_button_input.hpp"
#include "ilemu/touch_input.hpp"

struct SDL_Window;
union SDL_Event;

namespace ilemu {

class SdlInput {
public:
  explicit SdlInput(DisplayGeometry geometry)
      : geometry_{geometry}, display_geometry_{geometry} {}
  void set_orientation(DisplayOrientation orientation) {
    orientation_ = orientation;
  }
  void set_display_geometry(DisplayGeometry geometry) {
    if (geometry.valid())
      display_geometry_ = geometry;
  }
  [[nodiscard]] bool poll(SDL_Window *window);
  [[nodiscard]] bool wait(SDL_Window *window, std::chrono::nanoseconds timeout);
  [[nodiscard]] std::vector<TouchInput> take_touch_events();
  [[nodiscard]] std::vector<SystemButtonInput> take_button_events();
  [[nodiscard]] std::vector<RingerSwitchInput>
  take_ringer_switch_events();
  // A compositor may discard the window back buffer while it is hidden or
  // covered. The presenter consumes this edge to repaint the last scanout
  // without requiring a new guest frame.
  [[nodiscard]] bool take_redraw_request() {
    const auto requested = redraw_requested_;
    redraw_requested_ = false;
    return requested;
  }

private:
  void process_event(const SDL_Event &event, int window_width,
                     int window_height, DisplayViewport viewport);

  DisplayGeometry geometry_;
  DisplayGeometry display_geometry_;
  DisplayOrientation orientation_{DisplayOrientation::Portrait};
  std::vector<TouchInput> touch_events_;
  std::vector<SystemButtonInput> button_events_;
  std::vector<RingerSwitchInput> ringer_switch_events_;
  bool redraw_requested_{};
  bool mouse_active_{};
  std::unordered_set<std::int64_t> active_fingers_;
  bool running_{true};
};

} // namespace ilemu
