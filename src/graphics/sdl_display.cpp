#include "ilemu/sdl_display.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ilemu/display.hpp"
#include "sdl_input.hpp"

#if defined(ILEMU_HAS_SDL2)
#include <SDL.h>
#if defined(ILEMU_HAS_VULKAN)
#include <SDL_vulkan.h>
#endif
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
  bool vulkan_library_loaded{};
  bool vulkan_window{};
  bool surface_created{};

  void ensure_cpu_presenter() {
    if (renderer != nullptr && texture != nullptr)
      return;
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer == nullptr)
      renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (renderer == nullptr && vulkan_window && !surface_created) {
      SDL_DestroyWindow(window);
      window = SDL_CreateWindow(
          "iLEmu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
          static_cast<int>(geometry.width), static_cast<int>(geometry.height),
          SDL_WINDOW_RESIZABLE);
      vulkan_window = false;
      if (window != nullptr)
        renderer =
            SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
      if (renderer == nullptr && window != nullptr)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer == nullptr) {
      throw std::runtime_error{"SDL renderer creation failed: " +
                               std::string{SDL_GetError()}};
    }
    texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        static_cast<int>(geometry.width),
        static_cast<int>(geometry.height));
    if (texture == nullptr) {
      throw std::runtime_error{"SDL texture creation failed: " +
                               std::string{SDL_GetError()}};
    }
  }
#endif
  std::mutex frame_mutex;
  std::optional<DisplayFrame> pending_frame;
  std::shared_ptr<HostGraphicsDevice> host_graphics;
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
#if defined(ILEMU_HAS_VULKAN)
  if (SDL_Vulkan_LoadLibrary(nullptr) == 0) {
    impl_->vulkan_library_loaded = true;
    impl_->window = SDL_CreateWindow(
        "iLEmu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        static_cast<int>(impl_->geometry.width),
        static_cast<int>(impl_->geometry.height),
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    impl_->vulkan_window = impl_->window != nullptr;
  }
#endif
  if (impl_->window == nullptr) {
    impl_->window = SDL_CreateWindow(
        "iLEmu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        static_cast<int>(impl_->geometry.width),
        static_cast<int>(impl_->geometry.height), SDL_WINDOW_RESIZABLE);
    impl_->vulkan_window = false;
  }
  if (impl_->window == nullptr) {
    throw std::runtime_error{"SDL window creation failed: " +
                             std::string{SDL_GetError()}};
  }
  if (!impl_->vulkan_window)
    impl_->ensure_cpu_presenter();
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
#if defined(ILEMU_HAS_VULKAN)
    if (impl_->vulkan_library_loaded)
      SDL_Vulkan_UnloadLibrary();
#endif
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
#endif
}

std::optional<VulkanPresenterConfiguration>
SdlDisplay::vulkan_presenter_configuration() const {
#if defined(ILEMU_HAS_SDL2) && defined(ILEMU_HAS_VULKAN)
  if (!impl_->vulkan_window)
    return std::nullopt;
  unsigned int count{};
  if (SDL_Vulkan_GetInstanceExtensions(impl_->window, &count, nullptr) !=
          SDL_TRUE ||
      count == 0) {
    return std::nullopt;
  }
  std::vector<const char*> names(count);
  if (SDL_Vulkan_GetInstanceExtensions(impl_->window, &count,
                                       names.data()) != SDL_TRUE) {
    return std::nullopt;
  }
  VulkanPresenterConfiguration configuration;
  configuration.instance_extensions.reserve(count);
  for (const auto* name : names)
    configuration.instance_extensions.emplace_back(name);
  auto* implementation = impl_.get();
  configuration.create_surface = [implementation](std::uintptr_t instance) {
    VkSurfaceKHR surface{};
    if (SDL_Vulkan_CreateSurface(
            implementation->window,
            reinterpret_cast<VkInstance>(instance), &surface) != SDL_TRUE) {
      return std::uintptr_t{};
    }
    implementation->surface_created = true;
    return reinterpret_cast<std::uintptr_t>(surface);
  };
  configuration.drawable_size = [implementation] {
    int width{};
    int height{};
    SDL_Vulkan_GetDrawableSize(implementation->window, &width, &height);
    return std::pair{
        width > 0 ? static_cast<std::uint32_t>(width) : 0U,
        height > 0 ? static_cast<std::uint32_t>(height) : 0U};
  };
  return configuration;
#else
  return std::nullopt;
#endif
}

void SdlDisplay::set_host_graphics(
    std::shared_ptr<HostGraphicsDevice> graphics) {
  impl_->host_graphics = std::move(graphics);
#if defined(ILEMU_HAS_SDL2)
  if (impl_->host_graphics &&
      !impl_->host_graphics->native_presentation_available()) {
    impl_->ensure_cpu_presenter();
  }
#endif
}

void SdlDisplay::present(const DisplayFrame &frame) {
#if defined(ILEMU_HAS_SDL2)
  if (frame.width != impl_->geometry.width ||
      frame.height != impl_->geometry.height ||
      (frame.pixels.empty() && !frame.read_pixels)) {
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
    bool presented{};
    if (frame->host_surface && impl_->host_graphics &&
        impl_->host_graphics->native_presentation_available()) {
      presented = impl_->host_graphics->present(frame->host_surface);
    }
    if (!presented) {
    const auto expected =
        static_cast<std::size_t>(frame->width) * frame->height;
    if (frame->pixels.size() != expected && frame->read_pixels)
      frame->pixels = frame->read_pixels();
    if (frame->pixels.size() == expected) {
      impl_->ensure_cpu_presenter();
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
    }
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
