#include "ilemu/sdl_display.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ilemu/display.hpp"
#include "ilemu/gles_renderer.hpp"
#include "ilemu/performance.hpp"
#include "sdl_input.hpp"

#if defined(ILEMU_HAS_SDL2)
#include <SDL.h>
#if defined(ILEMU_HAS_VULKAN)
#include <SDL_vulkan.h>
#endif
#endif

namespace ilemu {
namespace {

// Guest owners and firmware surface identifiers are narrower than the host
// surface key. Reserve this owner for frames synthesized by the SDL presenter
// itself, such as the black panel emitted while display power is off.
constexpr auto sdl_presenter_surface_owner =
    std::numeric_limits<std::uint64_t>::max();
std::atomic<std::uint64_t> next_sdl_presenter_surface{1};

} // namespace

struct SdlDisplay::Impl {
  Impl(DisplayGeometry initial_geometry, DisplayGeometry input_geometry)
      : geometry{initial_geometry},
        cpu_present_surface_key{
            sdl_presenter_surface_owner,
            next_sdl_presenter_surface.fetch_add(1,
                                                 std::memory_order_relaxed)},
        input{input_geometry} {}

  DisplayGeometry geometry;
  HostSurfaceKey cpu_present_surface_key;
#if defined(ILEMU_HAS_SDL2)
  SDL_Window *window{};
  SDL_Renderer *renderer{};
  SDL_Texture *texture{};
  bool vulkan_library_loaded{};
  bool vulkan_window{};
  std::atomic<bool> surface_created{};

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
  std::condition_variable presentation_available;
  std::optional<DisplayFrame> pending_frame;
  std::optional<DisplayFrame> pending_native_frame;
  std::optional<DisplayFrame> failed_native_frame;
  std::shared_ptr<HostGraphicsDevice> host_graphics;
  std::shared_ptr<HostSurface> cpu_present_surface;
  std::thread presentation_thread;
  bool presentation_stopping{};
  SdlInput input;
  bool running{true};

  bool stage_cpu_frame_for_native_present(DisplayFrame &frame) {
    if (frame.host_surface || !host_graphics ||
        !host_graphics->native_presentation_available()) {
      return frame.host_surface != nullptr;
    }
    const auto expected =
        static_cast<std::size_t>(frame.width) * frame.height;
    if (frame.pixels.size() != expected && frame.read_pixels)
      frame.pixels = frame.read_pixels();
    if (frame.pixels.size() != expected)
      return false;
    if (!cpu_present_surface) {
      cpu_present_surface = host_graphics->create_surface(
          cpu_present_surface_key,
          HostSurfaceDescriptor{
              frame.width, frame.height,
              frame.width * static_cast<std::uint32_t>(sizeof(std::uint32_t)),
              0U, PerfSurfaceKind::Scanout},
          frame.pixels);
    } else {
      const auto descriptor = cpu_present_surface->descriptor();
      if (descriptor.width != frame.width ||
          descriptor.height != frame.height) {
        cpu_present_surface = host_graphics->create_surface(
            cpu_present_surface_key,
            HostSurfaceDescriptor{
                frame.width, frame.height,
                frame.width *
                    static_cast<std::uint32_t>(sizeof(std::uint32_t)),
                0U, PerfSurfaceKind::Scanout},
            frame.pixels);
      } else {
        cpu_present_surface->replace_cpu(frame.pixels);
      }
    }
    frame.host_surface = cpu_present_surface;
    return frame.host_surface != nullptr;
  }

  void start_native_presenter() {
    if (presentation_thread.joinable())
      return;
    presentation_stopping = false;
    presentation_thread = std::thread([this] {
      std::unique_lock lock{frame_mutex};
      while (true) {
        presentation_available.wait(lock, [this] {
          return presentation_stopping || pending_native_frame.has_value();
        });
        if (presentation_stopping)
          return;
        auto frame = std::move(*pending_native_frame);
        pending_native_frame.reset();
        auto graphics = host_graphics;
        lock.unlock();
        const auto result = graphics && frame.host_surface
                                ? graphics->present(frame.host_surface)
                                : HostGraphicsDevice::PresentResult::Failed;
        if (result == HostGraphicsDevice::PresentResult::Queued)
          performance_counters().record_native_present();
        lock.lock();
        if (result == HostGraphicsDevice::PresentResult::Failed)
          failed_native_frame = std::move(frame);
      }
    });
  }

  void stop_native_presenter() {
    {
      std::lock_guard lock{frame_mutex};
      presentation_stopping = true;
      pending_native_frame.reset();
    }
    presentation_available.notify_all();
    if (presentation_thread.joinable())
      presentation_thread.join();
    {
      std::lock_guard lock{frame_mutex};
      presentation_stopping = false;
    }
  }
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
    impl_->stop_native_presenter();
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
  impl_->stop_native_presenter();
  {
    std::lock_guard lock{impl_->frame_mutex};
    impl_->host_graphics = std::move(graphics);
    impl_->cpu_present_surface.reset();
  }
#if defined(ILEMU_HAS_SDL2)
  if (impl_->host_graphics &&
      !impl_->host_graphics->native_presentation_available()) {
    impl_->ensure_cpu_presenter();
  } else if (impl_->host_graphics) {
    impl_->start_native_presenter();
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
  // SDL events are latency-sensitive and must be drained before any CPU
  // fallback upload. Native presentation is handed to a lossy mailbox worker
  // and never waits on acquire/present from the guest scheduler thread.
  impl_->running = impl_->input.poll(impl_->window);
  std::optional<DisplayFrame> frame;
  bool native_failed{};
  {
    std::lock_guard lock{impl_->frame_mutex};
    if (impl_->failed_native_frame) {
      frame = std::move(impl_->failed_native_frame);
      impl_->failed_native_frame.reset();
      native_failed = true;
      if (impl_->pending_frame) {
        frame = std::move(impl_->pending_frame);
        impl_->pending_frame.reset();
      }
    } else {
      frame.swap(impl_->pending_frame);
    }
  }
  if (frame) {
    if (!native_failed)
      static_cast<void>(impl_->stage_cpu_frame_for_native_present(*frame));
    if (!native_failed && frame->host_surface && impl_->host_graphics &&
        impl_->host_graphics->native_presentation_available()) {
      {
        std::lock_guard lock{impl_->frame_mutex};
        impl_->pending_native_frame = std::move(*frame);
      }
      impl_->presentation_available.notify_one();
      frame.reset();
    }
  }
  if (frame) {
    if (frame->host_surface) {
      if (const auto renderer = std::dynamic_pointer_cast<GlesRenderer>(
              impl_->host_graphics)) {
        performance_counters().record_fallback(renderer->failure_reason());
      }
    }
    const auto expected =
        static_cast<std::size_t>(frame->width) * frame->height;
    if (frame->pixels.size() != expected && frame->read_pixels)
      frame->pixels = frame->read_pixels();
    if (frame->pixels.size() == expected) {
      performance_counters().record_cpu_present_fallback();
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
