#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "ilemu/display_geometry.hpp"
#include "ilemu/gles_renderer.hpp"
#include "ilemu/ringer_switch_state.hpp"
#include "ilemu/system_button_input.hpp"
#include "ilemu/touch_input.hpp"

namespace ilemu {

struct DisplayFrame;

class SdlDisplay {
public:
  SdlDisplay(DisplayGeometry frame_geometry,
             DisplayGeometry input_geometry);
  ~SdlDisplay();
  SdlDisplay(const SdlDisplay &) = delete;
  SdlDisplay &operator=(const SdlDisplay &) = delete;

  [[nodiscard]] static bool available();
  [[nodiscard]] std::optional<VulkanPresenterConfiguration>
  vulkan_presenter_configuration() const;
  void set_host_graphics(std::shared_ptr<HostGraphicsDevice> graphics);
  void present(DisplayFrame frame);
  void flush_presentation();
  // Counts frames accepted by the native swapchain, or completed by the SDL
  // software presenter.
  [[nodiscard]] std::uint64_t presented_frames() const;
  // Returns false after the user closes the window.
  [[nodiscard]] bool poll_events();
  // Blocks on the SDL event queue until an event or the supplied deadline.
  // The caller remains responsible for processing Guest deadlines.
  [[nodiscard]] bool wait_for_event(std::chrono::nanoseconds timeout);
  [[nodiscard]] std::vector<TouchInput> take_touch_events();
  [[nodiscard]] std::vector<SystemButtonInput> take_button_events();
  [[nodiscard]] std::vector<RingerSwitchInput>
  take_ringer_switch_events();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ilemu
