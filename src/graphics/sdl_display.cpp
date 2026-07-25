#include "ilemu/sdl_display.hpp"

#include <mutex>
#include <optional>
#include <stdexcept>

#include "ilemu/display.hpp"
#include "sdl_input.hpp"

#if defined(ILEMU_HAS_SDL2)
#include <SDL.h>
#endif

namespace ilemu {

struct SdlDisplay::Impl {
  Impl(DisplayGeometry initial_geometry, DisplayGeometry input_geometry)
      : geometry{initial_geometry}, input{input_geometry} {}

  DisplayGeometry geometry;
#if defined(ILEMU_HAS_SDL2)
  SDL_Window *window{};
  SDL_Renderer *renderer{};
  SDL_Texture *texture{};
#endif
  std::mutex frame_mutex;
  std::optional<DisplayFrame> pending_frame;
  SdlInput input;
  bool running{true};
};

bool SdlDisplay::available() {
#if defined(ILEMU_HAS_SDL2)
  return true;
#else
  return false;
#endif
}

SdlDisplay::SdlDisplay(DisplayGeometry frame_geometry,
                       DisplayGeometry input_geometry)
    : impl_{std::make_unique<Impl>(
          frame_geometry.valid() ? frame_geometry : default_display_geometry,
          input_geometry.valid() ? input_geometry
                                 : default_display_geometry)} {
#if defined(ILEMU_HAS_SDL2)
  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    throw std::runtime_error{"SDL video initialization failed: " +
                             std::string{SDL_GetError()}};
  }
  impl_->window = SDL_CreateWindow(
      "iLEmu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      static_cast<int>(impl_->geometry.width),
      static_cast<int>(impl_->geometry.height), SDL_WINDOW_RESIZABLE);
  if (impl_->window == nullptr) {
    throw std::runtime_error{"SDL window creation failed: " +
                             std::string{SDL_GetError()}};
  }
  impl_->renderer = SDL_CreateRenderer(
      impl_->window, -1, SDL_RENDERER_ACCELERATED);
  if (impl_->renderer == nullptr) {
    impl_->renderer =
        SDL_CreateRenderer(impl_->window, -1, SDL_RENDERER_SOFTWARE);
  }
  if (impl_->renderer == nullptr) {
    throw std::runtime_error{"SDL renderer creation failed: " +
                             std::string{SDL_GetError()}};
  }
  impl_->texture = SDL_CreateTexture(
      impl_->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      static_cast<int>(impl_->geometry.width),
      static_cast<int>(impl_->geometry.height));
  if (impl_->texture == nullptr) {
    throw std::runtime_error{"SDL texture creation failed: " +
                             std::string{SDL_GetError()}};
  }
  DisplayFrame initial{impl_->geometry.width, impl_->geometry.height, 0,
                       std::vector<std::uint32_t>(impl_->geometry.pixel_count(),
                                                  0xff000000U)};
  present(initial);
#else
  throw std::runtime_error{
      "SDL2 display support was not available when iLEmu was built"};
#endif
}

SdlDisplay::~SdlDisplay() {
#if defined(ILEMU_HAS_SDL2)
  if (impl_) {
    if (impl_->texture != nullptr)
      SDL_DestroyTexture(impl_->texture);
    if (impl_->renderer != nullptr)
      SDL_DestroyRenderer(impl_->renderer);
    if (impl_->window != nullptr)
      SDL_DestroyWindow(impl_->window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
#endif
}

void SdlDisplay::present(const DisplayFrame &frame) {
#if defined(ILEMU_HAS_SDL2)
  if (frame.width != impl_->geometry.width ||
      frame.height != impl_->geometry.height || frame.pixels.empty()) {
    return;
  }
  std::lock_guard lock{impl_->frame_mutex};
  impl_->pending_frame = frame;
#else
  static_cast<void>(frame);
#endif
}

bool SdlDisplay::poll_events() {
#if defined(ILEMU_HAS_SDL2)
  std::optional<DisplayFrame> frame;
  {
    std::lock_guard lock{impl_->frame_mutex};
    frame.swap(impl_->pending_frame);
  }
  if (frame) {
    if (SDL_UpdateTexture(
            impl_->texture, nullptr, frame->pixels.data(),
            static_cast<int>(frame->width * sizeof(std::uint32_t))) != 0) {
      throw std::runtime_error{"SDL texture upload failed: " +
                               std::string{SDL_GetError()}};
    }
    SDL_RenderClear(impl_->renderer);
    SDL_RenderCopy(impl_->renderer, impl_->texture, nullptr, nullptr);
    SDL_RenderPresent(impl_->renderer);
  }
  impl_->running = impl_->input.poll(impl_->window);
#endif
  return impl_->running;
}

std::vector<TouchInput> SdlDisplay::take_touch_events() {
  return impl_->input.take_touch_events();
}

std::vector<SystemButtonInput> SdlDisplay::take_button_events() {
  return impl_->input.take_button_events();
}

std::vector<RingerSwitchInput> SdlDisplay::take_ringer_switch_events() {
  return impl_->input.take_ringer_switch_events();
}

} // namespace ilemu
