#include "ilemu/sdl_display.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ilemu/display.hpp"
#include "ilemu/application_display_profile.hpp"
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
        guest_geometry{initial_geometry},
        cpu_present_surface_key{
            sdl_presenter_surface_owner,
            next_sdl_presenter_surface.fetch_add(1,
                                                 std::memory_order_relaxed)},
        input{input_geometry} {
    input.set_display_geometry(geometry);
  }

  DisplayGeometry geometry;
  DisplayGeometry guest_geometry;
  DisplayOrientation orientation{DisplayOrientation::Portrait};
  HostSurfaceKey cpu_present_surface_key;
  HostSurfaceKey oriented_present_surface_key{
      sdl_presenter_surface_owner,
      next_sdl_presenter_surface.fetch_add(1,
                                           std::memory_order_relaxed)};
#if defined(ILEMU_HAS_SDL2)
  SDL_Window *window{};
  SDL_Renderer *renderer{};
  SDL_Texture *texture{};
  SDL_Window *retired_window{};
  bool vulkan_library_loaded{};
  bool vulkan_window{};
  std::atomic<bool> surface_created{};

  void ensure_cpu_presenter() {
    if (renderer == nullptr) {
      SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
      renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
      if (renderer == nullptr)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
      if (renderer == nullptr && vulkan_window && !surface_created) {
        SDL_DestroyWindow(window);
        window = SDL_CreateWindow(
            "iLEmu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            static_cast<int>(geometry.width),
            static_cast<int>(geometry.height), SDL_WINDOW_RESIZABLE);
        vulkan_window = false;
        if (window != nullptr)
          renderer =
              SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (renderer == nullptr && window != nullptr)
          renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
      }
    }
    if (renderer == nullptr) {
      throw std::runtime_error{"SDL renderer creation failed: " +
                               std::string{SDL_GetError()}};
    }
    if (texture != nullptr)
      return;
    texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        static_cast<int>(geometry.width),
        static_cast<int>(geometry.height));
    if (texture == nullptr) {
      throw std::runtime_error{"SDL texture creation failed: " +
                               std::string{SDL_GetError()}};
    }
  }

  // Wayland configures a toplevel's size asynchronously and does not
  // generally honor SDL_SetWindowSize after the initial configure. An
  // orientation change therefore needs a fresh toplevel so its initial
  // request carries the new aspect ratio. The native presenter is rebound by
  // the caller while the previous toplevel is still alive.
  bool recreate_wayland_window() {
    if (window == nullptr ||
        std::string_view{SDL_GetCurrentVideoDriver() != nullptr
                             ? SDL_GetCurrentVideoDriver()
                             : ""} != "wayland") {
      return false;
    }
    if (retired_window != nullptr) {
      SDL_DestroyWindow(retired_window);
      retired_window = nullptr;
    }
    const auto old_flags = SDL_GetWindowFlags(window);
    const auto new_flags =
        SDL_WINDOW_RESIZABLE |
        (vulkan_window ? static_cast<Uint32>(SDL_WINDOW_VULKAN) : 0U) |
        (old_flags & SDL_WINDOW_ALLOW_HIGHDPI);
    auto *replacement = SDL_CreateWindow(
        "iLEmu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        static_cast<int>(geometry.width), static_cast<int>(geometry.height),
        new_flags);
    if (replacement == nullptr)
      return false;

    SDL_Renderer *replacement_renderer = nullptr;
    if (!vulkan_window) {
      SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
      replacement_renderer =
          SDL_CreateRenderer(replacement, -1, SDL_RENDERER_ACCELERATED);
      if (replacement_renderer == nullptr)
        replacement_renderer =
            SDL_CreateRenderer(replacement, -1, SDL_RENDERER_SOFTWARE);
      if (replacement_renderer == nullptr) {
        SDL_DestroyWindow(replacement);
        return false;
      }
    }

    auto *old_window = window;
    if (texture != nullptr)
      SDL_DestroyTexture(texture);
    if (renderer != nullptr)
      SDL_DestroyRenderer(renderer);
    window = replacement;
    renderer = replacement_renderer;
    texture = nullptr;
    surface_created = false;
    // Keep the old toplevel alive until the Vulkan backend has released its
    // surface. Wayland display queues may still contain protocol work for the
    // previous surface while the guest orientation transition is handled.
    retired_window = old_window;
    if (!vulkan_window)
      ensure_cpu_presenter();
    return true;
  }

  void destroy_retired_window() {
    if (retired_window != nullptr) {
      SDL_DestroyWindow(retired_window);
      retired_window = nullptr;
    }
  }
#endif
  std::mutex frame_mutex;
  std::condition_variable presentation_available;
  std::condition_variable presentation_idle;
  std::optional<DisplayFrame> pending_frame;
  std::optional<DisplayFrame> pending_native_frame;
  std::optional<DisplayFrame> failed_native_frame;
  // SDL windows can lose their compositor back buffer while hidden or
  // covered. Retain the last host-ready frame so an expose/restore event can
  // repaint without waiting for the guest to submit another frame.
  std::optional<DisplayFrame> last_presented_frame;
  std::shared_ptr<HostGraphicsDevice> host_graphics;
  std::shared_ptr<HostSurface> cpu_present_surface;
  std::shared_ptr<HostSurface> oriented_present_surface;
  std::unique_ptr<CommandEncoder> orientation_encoder;
  std::thread presentation_thread;
  std::atomic<std::uint64_t> presented_frames{};
  bool presentation_stopping{};
  bool presentation_active{};
  SdlInput input;
  bool running{true};

  void update_orientation(DisplayOrientation next_orientation) {
    if (orientation == next_orientation)
      return;
    orientation = next_orientation;
    geometry = is_landscape(orientation)
                   ? DisplayGeometry{guest_geometry.height,
                                     guest_geometry.width}
                   : guest_geometry;
    input.set_display_geometry(geometry);
    input.set_orientation(orientation);
#if defined(ILEMU_HAS_SDL2)
    if (window != nullptr) {
      const auto native_presentation =
          host_graphics && host_graphics->native_presentation_available();
      if (native_presentation)
        stop_native_presenter();
      if (recreate_wayland_window()) {
        if (native_presentation) {
          static_cast<void>(host_graphics->refresh_presentation_surface());
          start_native_presenter();
        }
        destroy_retired_window();
      } else {
        SDL_SetWindowSize(window, static_cast<int>(geometry.width),
                          static_cast<int>(geometry.height));
        if (native_presentation)
          start_native_presenter();
      }
    }
    if (texture != nullptr) {
      SDL_DestroyTexture(texture);
      texture = nullptr;
    }
#endif
  }

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

  bool stage_oriented_frame_for_native_present(
      DisplayFrame &frame, DisplayOrientation requested_orientation) {
    if (!frame.host_surface || !host_graphics || !orientation_encoder ||
        !host_graphics->native_presentation_available() ||
        requested_orientation == DisplayOrientation::Portrait) {
      return false;
    }
    const auto source = frame.host_surface;
    const auto source_descriptor = source->descriptor();
    if (source_descriptor.width != frame.width ||
        source_descriptor.height != frame.height) {
      return false;
    }
    const auto output_geometry =
        is_landscape(requested_orientation)
            ? DisplayGeometry{frame.height, frame.width}
            : DisplayGeometry{frame.width, frame.height};
    if (!oriented_present_surface ||
        oriented_present_surface->descriptor().width !=
            output_geometry.width ||
        oriented_present_surface->descriptor().height !=
            output_geometry.height) {
      oriented_present_surface = host_graphics->create_surface(
          oriented_present_surface_key,
          HostSurfaceDescriptor{
              output_geometry.width, output_geometry.height,
              output_geometry.width *
                  static_cast<std::uint32_t>(sizeof(std::uint32_t)),
              source_descriptor.pixel_format,
              PerfSurfaceKind::Scanout});
    }
    if (!oriented_present_surface)
      return false;

    const auto source_width = static_cast<float>(frame.width);
    const auto source_height = static_cast<float>(frame.height);
    const auto output_width = static_cast<float>(output_geometry.width);
    const auto output_height = static_cast<float>(output_geometry.height);
    std::array<HostPoint, 4> texture_coordinates{};
    switch (requested_orientation) {
    case DisplayOrientation::Portrait:
      return false;
    case DisplayOrientation::PortraitUpsideDown:
      texture_coordinates = {{{source_width, source_height},
                              {0.0F, source_height},
                              {0.0F, 0.0F},
                              {source_width, 0.0F}}};
      break;
    case DisplayOrientation::LandscapeLeft:
      texture_coordinates = {{{0.0F, 0.0F},
                              {0.0F, source_height},
                              {source_width, source_height},
                              {source_width, 0.0F}}};
      break;
    case DisplayOrientation::LandscapeRight:
      texture_coordinates = {{{source_width, source_height},
                              {source_width, 0.0F},
                              {0.0F, 0.0F},
                              {0.0F, source_height}}};
      break;
    }
    const std::array<HostPoint, 4> positions{{
        {0.0F, 0.0F},
        {output_width, 0.0F},
        {output_width, output_height},
        {0.0F, output_height},
    }};
    std::array<HostTexturedVertex, 4> quad{};
    for (std::size_t index = 0; index < quad.size(); ++index)
      quad[index] = {positions[index], texture_coordinates[index]};
    const auto source_rectangle = HostRectangle{
        0, 0, frame.width, frame.height};
    const auto destination_rectangle = HostRectangle{
        0, 0, output_geometry.width, output_geometry.height};
    if (!orientation_encoder->copy_quad(
            source, oriented_present_surface, quad, source_rectangle,
            destination_rectangle, HostCompositeMode::Copy, 0xffU,
            HostFilter::Nearest) ||
        !orientation_encoder->submit(PerfSubmitReason::Presentation)) {
      return false;
    }

    auto graphics = host_graphics;
    auto oriented = oriented_present_surface;
    frame.width = output_geometry.width;
    frame.height = output_geometry.height;
    frame.pixels.clear();
    frame.host_surface = oriented;
    frame.read_pixels = [graphics = std::move(graphics),
                         oriented = std::move(oriented)] {
      if (!graphics->map_cpu(*oriented, true,
                             PerfCpuMapReason::DeferredDisplayRead)) {
        return std::vector<std::uint32_t>{};
      }
      auto mapping = oriented->map_cpu(
          false, PerfCpuMapReason::DeferredDisplayRead);
      return mapping.frame().pixels;
    };
    frame.orientation = DisplayOrientation::Portrait;
    return true;
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
        presentation_active = true;
        auto graphics = host_graphics;
        lock.unlock();
        auto &performance = performance_counters();
        const auto telemetry_enabled = performance.enabled();
        const auto native_dequeued_at =
            telemetry_enabled ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{};
        if (telemetry_enabled) {
          performance.record_diagnostic_native_dequeue(
              frame.sequence, native_dequeued_at);
        }
        if (telemetry_enabled && frame.native_queued_at !=
            std::chrono::steady_clock::time_point{}) {
          const auto residence =
              native_dequeued_at - frame.native_queued_at;
          performance.record_latency(
              PerfLatencyKind::NativeMailbox,
              static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      residence)
                      .count()));
        }
        if (telemetry_enabled)
          performance.record_native_present_attempt();
        const auto result = graphics && frame.host_surface
                                ? graphics->present(frame.host_surface)
                                : HostGraphicsDevice::PresentResult::Failed;
        if (result == HostGraphicsDevice::PresentResult::Queued) {
          presented_frames.fetch_add(1, std::memory_order_release);
          performance.record_native_present(
              frame.sequence, frame.submitted_at);
        } else if (result == HostGraphicsDevice::PresentResult::Skipped) {
          performance.record_native_present_skipped();
        } else {
          performance.record_native_present_failure();
        }
        lock.lock();
        presentation_active = false;
        if (result == HostGraphicsDevice::PresentResult::Failed)
          failed_native_frame = std::move(frame);
        presentation_idle.notify_all();
      }
    });
  }

  void stop_native_presenter() {
    {
      std::lock_guard lock{frame_mutex};
      presentation_stopping = true;
      if (pending_native_frame)
        performance_counters().record_native_present_mailbox_coalesced();
      pending_native_frame.reset();
    }
    presentation_available.notify_all();
    if (presentation_thread.joinable())
      presentation_thread.join();
    {
      std::lock_guard lock{frame_mutex};
      presentation_stopping = false;
      presentation_active = false;
    }
  }

  void flush_native_presenter() {
    std::unique_lock lock{frame_mutex};
    if (!presentation_thread.joinable())
      return;
    presentation_idle.wait(lock, [this] {
      return !pending_native_frame && !presentation_active;
    });
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
    impl_->destroy_retired_window();
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
    // Release frames before dropping the renderer. A deferred readback keeps
    // a shared host-graphics reference alive; retaining it until Impl is
    // destroyed would let Vulkan tear down a Wayland surface after SDL has
    // already destroyed its window.
    impl_->pending_frame.reset();
    impl_->pending_native_frame.reset();
    impl_->failed_native_frame.reset();
    impl_->last_presented_frame.reset();
    impl_->host_graphics = std::move(graphics);
    impl_->cpu_present_surface.reset();
    impl_->oriented_present_surface.reset();
    impl_->orientation_encoder =
        impl_->host_graphics
            ? impl_->host_graphics->create_command_encoder()
            : nullptr;
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
  if (frame.width != impl_->guest_geometry.width ||
      frame.height != impl_->guest_geometry.height ||
      (frame.pixels.empty() && !frame.read_pixels)) {
    return;
  }
  std::lock_guard lock{impl_->frame_mutex};
  if (impl_->pending_frame)
    performance_counters().record_display_mailbox_coalesced();
  impl_->pending_frame = frame;
#else
  static_cast<void>(frame);
#endif
}

void SdlDisplay::flush_presentation() {
#if defined(ILEMU_HAS_SDL2)
  // A native failure is converted to the software path by poll_events(), so a
  // complete boundary has to alternate the main-thread pump with the native
  // worker until every stage is empty.
  while (true) {
    static_cast<void>(poll_events());
    impl_->flush_native_presenter();
    std::lock_guard lock{impl_->frame_mutex};
    if (!impl_->pending_frame && !impl_->pending_native_frame &&
        !impl_->failed_native_frame && !impl_->presentation_active) {
      break;
    }
  }
#endif
}

std::uint64_t SdlDisplay::presented_frames() const {
  return impl_->presented_frames.load(std::memory_order_acquire);
}

bool SdlDisplay::poll_events() {
#if defined(ILEMU_HAS_SDL2)
  // SDL events are latency-sensitive and must be drained before any CPU
  // fallback upload. Native presentation is handed to a lossy mailbox worker
  // and never waits on acquire/present from the guest scheduler thread.
  impl_->running = impl_->input.poll(impl_->window);
  const auto redraw_requested = impl_->input.take_redraw_request();
  std::optional<DisplayFrame> frame;
  bool native_failed{};
  bool repainting{};
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
    if (!frame && redraw_requested && impl_->last_presented_frame) {
      frame = *impl_->last_presented_frame;
      repainting = true;
    }
  }
  if (frame) {
    const auto requested_orientation = frame->orientation;
    if (requested_orientation != DisplayOrientation::Portrait) {
      const auto native_oriented =
          !native_failed &&
          impl_->stage_oriented_frame_for_native_present(
              *frame, requested_orientation);
      if (!native_oriented) {
        const auto raw_geometry =
            DisplayGeometry{frame->width, frame->height};
        const auto expected = raw_geometry.pixel_count();
        if (frame->pixels.size() != expected && frame->read_pixels)
          frame->pixels = frame->read_pixels();
        const auto oriented = orient_display_pixels(
            raw_geometry, frame->pixels, requested_orientation);
        if (oriented.empty()) {
          frame.reset();
        } else {
          frame->pixels = oriented;
          frame->host_surface.reset();
          frame->read_pixels = {};
          if (is_landscape(requested_orientation))
            std::swap(frame->width, frame->height);
          frame->orientation = DisplayOrientation::Portrait;
        }
      }
    }
    if (frame && !repainting && impl_->orientation != requested_orientation)
      impl_->update_orientation(requested_orientation);
  }
  if (frame) {
    auto &performance = performance_counters();
    const auto telemetry_enabled = performance.enabled();
    const auto display_dequeued_at =
        telemetry_enabled ? std::chrono::steady_clock::now()
                          : std::chrono::steady_clock::time_point{};
    if (telemetry_enabled) {
      performance.record_diagnostic_display_dequeue(
          frame->sequence, display_dequeued_at);
    }
    if (telemetry_enabled && frame->submitted_at !=
        std::chrono::steady_clock::time_point{}) {
      const auto residence =
          display_dequeued_at - frame->submitted_at;
      performance.record_latency(
          PerfLatencyKind::DisplayMailbox,
          static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(residence)
                  .count()));
    }
    if (!native_failed)
      static_cast<void>(impl_->stage_cpu_frame_for_native_present(*frame));
    if (!native_failed && frame->host_surface && impl_->host_graphics &&
        impl_->host_graphics->native_presentation_available()) {
      const auto native_sequence = frame->sequence;
      std::chrono::steady_clock::time_point native_queued_at;
      {
        std::lock_guard lock{impl_->frame_mutex};
        if (impl_->pending_native_frame) {
          performance_counters()
              .record_native_present_mailbox_coalesced();
        }
        impl_->last_presented_frame = *frame;
        if (telemetry_enabled)
          frame->native_queued_at = std::chrono::steady_clock::now();
        native_queued_at = frame->native_queued_at;
        impl_->pending_native_frame = std::move(*frame);
      }
      if (telemetry_enabled) {
        performance.record_diagnostic_native_queue(
            native_sequence, native_queued_at);
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
      impl_->ensure_cpu_presenter();
      if (SDL_UpdateTexture(
              impl_->texture, nullptr, frame->pixels.data(),
              static_cast<int>(frame->width * sizeof(std::uint32_t))) != 0) {
        throw std::runtime_error{"SDL texture upload failed: " +
                                 std::string{SDL_GetError()}};
      }
      int output_width{};
      int output_height{};
      if (SDL_GetRendererOutputSize(
              impl_->renderer, &output_width, &output_height) != 0) {
        throw std::runtime_error{
            "SDL renderer output query failed: " +
            std::string{SDL_GetError()}};
      }
      const auto viewport = fit_display_viewport(
          {frame->width, frame->height},
          {static_cast<std::uint32_t>(std::max(output_width, 0)),
           static_cast<std::uint32_t>(std::max(output_height, 0))});
      const SDL_Rect destination{
          viewport.x, viewport.y, static_cast<int>(viewport.width),
          static_cast<int>(viewport.height)};
      SDL_SetRenderDrawColor(impl_->renderer, 0U, 0U, 0U, 255U);
      SDL_RenderClear(impl_->renderer);
      SDL_RenderCopy(
          impl_->renderer, impl_->texture, nullptr, &destination);
      SDL_RenderPresent(impl_->renderer);
      {
        std::lock_guard lock{impl_->frame_mutex};
        impl_->last_presented_frame = *frame;
      }
      impl_->presented_frames.fetch_add(1, std::memory_order_release);
      performance_counters().record_cpu_present_fallback(
          frame->sequence, frame->submitted_at);
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
