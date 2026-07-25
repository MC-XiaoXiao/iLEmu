#include "ilemu/mobile_framebuffer_hle.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ilemu/address_space.hpp"
#include "ilemu/core_surface_abi.hpp"
#include "ilemu/display.hpp"
#include "ilemu/gles_renderer.hpp"
#include "ilemu/iokit_abi.hpp"
#include "ilemu/kernel_shared_state.hpp"
#include "ilemu/mobile_framebuffer_abi.hpp"
#include "ilemu/output.hpp"
#include "ilemu/presentation_tracker.hpp"
#include "ilemu/scene_coordinator.hpp"
#include "ilemu/surface_store.hpp"
#include "ilemu/userland_hle.hpp"

namespace ilemu {
namespace {

constexpr std::string_view framebuffer_image{
    "/System/Library/Frameworks/IOMobileFramebuffer.framework/"
    "IOMobileFramebuffer"};
std::atomic<std::uint64_t> next_scanout_surface{1};

} // namespace

MobileFramebufferHle::MobileFramebufferHle(
    UserlandHleRegistry &registry, std::shared_ptr<DisplayState> display,
    std::shared_ptr<SurfaceStore> surfaces,
    std::shared_ptr<PresentationTracker> presentations)
    : display_{std::move(display)},
      surface_store_{surfaces ? std::move(surfaces)
                              : std::make_shared<SurfaceStore>()},
      presentation_tracker_{
          presentations ? std::move(presentations)
                        : std::make_shared<PresentationTracker>()},
      host_graphics_{shared_gles_renderer()},
      command_encoder_{host_graphics_->create_command_encoder()} {
  const auto add = [&](std::string symbol,
                       UserlandHleRegistry::Handler handler) {
    registry.register_function(std::string{framebuffer_image},
                               std::move(symbol), std::move(handler));
  };
  // IOMobileFramebufferOpen intentionally executes from the firmware: it
  // allocates a genuine CFRuntime object and opens only our display handle.
  add("_IOMobileFramebufferGetDisplaySize", [this](UserlandHleCall &call) {
    const auto output = call.argument(1);
    const auto geometry = display_ ? display_->geometry()
                                   : default_display_geometry;
    const auto width = std::bit_cast<std::uint32_t>(
        static_cast<float>(geometry.width));
    const auto height = std::bit_cast<std::uint32_t>(
        static_cast<float>(geometry.height));
    call.set_return(output != 0 && call.memory().write32(output, width) &&
                            call.memory().write32(output + 4U, height)
                        ? iokit_abi::success
                        : iokit_abi::bad_argument);
  });
  add("_IOMobileFramebufferGetID", [](UserlandHleCall &call) {
    call.set_return(call.write32(call.argument(1), 0)
                        ? iokit_abi::success
                        : iokit_abi::bad_argument);
  });
  // GetLayerDefaultSurface intentionally remains firmware code. It calls
  // IOConnectCallScalarMethod(selector 3) and then CoreSurfaceBufferLookup,
  // preserving the real CFRuntime wrapper around our client-buffer HLE.
  add("_IOMobileFramebufferSwapBegin", [this](UserlandHleCall &call) {
    const auto swap_id = next_swap_id_++;
    call.set_return(call.write32(call.argument(1), swap_id)
                        ? iokit_abi::success
                        : iokit_abi::bad_argument);
  });
  add("_IOMobileFramebufferSwapSetBackgroundColor",
      [this](UserlandHleCall &call) { set_background_color(call); });
  add("_IOMobileFramebufferSwapEnd", [this](UserlandHleCall &call) {
    if (display_write_allowed(call)) {
      submit_layers(call);
      record_presentation(call);
      if (display_)
        display_->present();
    }
    call.set_return(iokit_abi::success);
  });
  add("_IOMobileFramebufferSwapSurface", [this](UserlandHleCall &call) {
    if (display_ && display_write_allowed(call))
      display_->present();
    call.set_return(iokit_abi::success);
  });
  const auto success = [](UserlandHleCall &call) {
    call.set_return(iokit_abi::success);
  };
  add("_IOMobileFramebufferWaitSurface", success);
  add("_IOMobileFramebufferEnableStatistics", success);
  // GetNotifyMessageCount remains firmware code. It asks the Mach port layer
  // for MACH_PORT_RECEIVE_STATUS and returns mps_msgcount so the native vsync
  // callback can coalesce queued notifications accurately.
  add("_IOMobileFramebufferSetDebugFlags", success);
  add("_IOMobileFramebufferSetTVOutMode", success);
  add("_IOMobileFramebufferSetWSSInfo", success);
  add("_IOMobileFramebufferSwapSetGammaTable", success);
  add("_IOMobileFramebufferSwapSetLayer",
      [this](UserlandHleCall &call) { set_layer(call); });
  add("_IOMobileFramebufferSwapWait", success);
  add("_IOMobileFramebufferCreateStatistics",
      [](UserlandHleCall &call) { call.set_return(0); });
  add("_IOMobileFramebufferGetTypeID",
      [](UserlandHleCall &call) { call.set_return(0); });
}

void MobileFramebufferHle::reset() {
  layers_.clear();
  next_swap_id_ = 1;
  background_argb_ = 0xff000000U;
}

void MobileFramebufferHle::inherit_state(const MobileFramebufferHle &parent) {
  layers_ = parent.layers_;
  next_swap_id_ = parent.next_swap_id_;
  background_argb_ = parent.background_argb_;
  scanout_surface_ = parent.scanout_surface_;
}

void MobileFramebufferHle::set_display(std::shared_ptr<DisplayState> display) {
  display_ = std::move(display);
  if (scanout_surface_ && display_) {
    const auto descriptor = scanout_surface_->descriptor();
    if (descriptor.width != display_->width() ||
        descriptor.height != display_->height()) {
      scanout_surface_.reset();
    }
  }
}

void MobileFramebufferHle::set_shared_state(
    std::shared_ptr<KernelSharedState> shared_state) {
  shared_state_ = std::move(shared_state);
}

void MobileFramebufferHle::set_presentation_tracker(
    std::shared_ptr<PresentationTracker> presentations) {
  presentation_tracker_ = std::move(presentations);
}

void MobileFramebufferHle::set_scene_coordinator(
    std::shared_ptr<SceneCoordinator> scenes) {
  scene_coordinator_ = std::move(scenes);
}

bool MobileFramebufferHle::display_write_allowed(
    UserlandHleCall &call) const {
  if (!shared_state_)
    return true;
  std::lock_guard lock{shared_state_->mach_mutex};
  const auto process = shared_state_->processes.find(call.process_id());
  if (process == shared_state_->processes.end() ||
      !process->second.executable_path.starts_with("/Applications/")) {
    return true;
  }
  return active_application_owns_display_locked(
      *shared_state_, call.process_id(),
      scene_coordinator_
          ? std::optional<bool>{
                scene_coordinator_->client_scene_active(call.process_id())}
          : std::nullopt);
}

bool MobileFramebufferHle::has_active_layers() const {
  return !layers_.empty();
}

void MobileFramebufferHle::ensure_scanout_surface() {
  if (!display_)
    return;
  if (scanout_surface_) {
    const auto descriptor = scanout_surface_->descriptor();
    if (descriptor.width == display_->width() &&
        descriptor.height == display_->height()) {
      return;
    }
  }
  const auto pixels =
      static_cast<std::size_t>(display_->width()) * display_->height();
  const std::vector<std::uint32_t> initial(pixels, background_argb_);
  scanout_surface_ = host_graphics_->create_surface(
      {0x4d464253U,
       next_scanout_surface.fetch_add(1, std::memory_order_relaxed)},
      HostSurfaceDescriptor{display_->width(), display_->height(),
                            display_->width() *
                                core_surface_abi::bytes_per_bgra_pixel,
                            surface_pixel_format_bgra},
      initial);
}

bool MobileFramebufferHle::submit_host_layers() {
  if (!display_ || !host_graphics_->accelerated() || !command_encoder_)
    return false;
  ensure_scanout_surface();
  if (!scanout_surface_ ||
      !command_encoder_->fill(
          scanout_surface_,
          {0, 0, display_->width(), display_->height()},
          background_argb_)) {
    return false;
  }
  const auto exact_rectangle = [](const Rectangle &rectangle)
      -> std::optional<HostRectangle> {
    const auto exact = [](float value) {
      return std::isfinite(value) && std::trunc(value) == value;
    };
    if (!exact(rectangle.x) || !exact(rectangle.y) ||
        !exact(rectangle.width) || !exact(rectangle.height) ||
        rectangle.x < 0.0F || rectangle.y < 0.0F ||
        rectangle.width <= 0.0F || rectangle.height <= 0.0F ||
        rectangle.x >
            static_cast<float>(std::numeric_limits<std::int32_t>::max()) ||
        rectangle.y >
            static_cast<float>(std::numeric_limits<std::int32_t>::max()) ||
        rectangle.width >
            static_cast<float>(std::numeric_limits<std::uint32_t>::max()) ||
        rectangle.height >
            static_cast<float>(std::numeric_limits<std::uint32_t>::max())) {
      return std::nullopt;
    }
    return HostRectangle{
        static_cast<std::int32_t>(rectangle.x),
        static_cast<std::int32_t>(rectangle.y),
        static_cast<std::uint32_t>(rectangle.width),
        static_cast<std::uint32_t>(rectangle.height)};
  };
  for (const auto &[layer, state] : layers_) {
    static_cast<void>(layer);
    const auto backing = surface_store_->find(state.surface_id);
    const auto source = surface_store_->host_surface(state.surface_id);
    const auto source_rectangle = exact_rectangle(state.source);
    const auto destination_rectangle = exact_rectangle(state.destination);
    if (!backing || backing->pixel_format != surface_pixel_format_bgra ||
        !source || !source_rectangle || !destination_rectangle ||
        static_cast<std::uint32_t>(source_rectangle->x) >
            backing->width - std::min(backing->width,
                                      source_rectangle->width) ||
        source_rectangle->width > backing->width ||
        static_cast<std::uint32_t>(source_rectangle->y) >
            backing->height - std::min(backing->height,
                                       source_rectangle->height) ||
        source_rectangle->height > backing->height ||
        static_cast<std::uint32_t>(destination_rectangle->x) >
            display_->width() -
                std::min(display_->width(), destination_rectangle->width) ||
        destination_rectangle->width > display_->width() ||
        static_cast<std::uint32_t>(destination_rectangle->y) >
            display_->height() -
                std::min(display_->height(),
                         destination_rectangle->height) ||
        destination_rectangle->height > display_->height() ||
        !command_encoder_->copy(
            source, scanout_surface_, *source_rectangle,
            *destination_rectangle,
            HostCompositeMode::PremultipliedSourceOver, 0xffU,
            HostFilter::Nearest)) {
      return false;
    }
  }
  if (!command_encoder_->submit())
    return false;
  auto graphics = host_graphics_;
  auto scanout = scanout_surface_;
  display_->replace_surface(
      scanout,
      [graphics, scanout] {
        if (!graphics->map_cpu(*scanout, true))
          return std::vector<std::uint32_t>{};
        auto mapping = scanout->map_cpu(false);
        return mapping.frame().pixels;
      });
  return true;
}

void MobileFramebufferHle::set_layer(UserlandHleCall &call) {
  const auto layer = call.argument(1);
  const auto surface = call.argument(2);
  if (layer > mobile_framebuffer_abi::maximum_layer_index) {
    call.set_return(iokit_abi::bad_argument);
    return;
  }
  if (surface == 0) {
    layers_.erase(layer);
    call.set_return(iokit_abi::success);
    return;
  }
  if (surface > std::numeric_limits<std::uint32_t>::max() -
                    core_surface_abi::public_client_buffer_offset) {
    call.set_return(iokit_abi::bad_argument);
    return;
  }
  const auto client = call.memory().read32(
      surface + core_surface_abi::public_client_buffer_offset);
  if (!client || *client == 0 ||
      *client > std::numeric_limits<std::uint32_t>::max() -
                    core_surface_abi::client_identifier_offset) {
    call.set_return(iokit_abi::bad_argument);
    return;
  }
  const auto identifier = call.memory().read32(
      *client + core_surface_abi::client_identifier_offset);
  if (!identifier || *identifier == 0 || !surface_store_->find(*identifier)) {
    call.set_return(iokit_abi::bad_argument);
    return;
  }
  const auto float_argument = [&](std::size_t index) {
    return std::bit_cast<float>(call.argument(index));
  };
  const Rectangle source{
      float_argument(mobile_framebuffer_abi::source_x_argument),
      float_argument(mobile_framebuffer_abi::source_y_argument),
      float_argument(mobile_framebuffer_abi::source_width_argument),
      float_argument(mobile_framebuffer_abi::source_height_argument)};
  const Rectangle destination{
      float_argument(mobile_framebuffer_abi::destination_x_argument),
      float_argument(mobile_framebuffer_abi::destination_y_argument),
      float_argument(mobile_framebuffer_abi::destination_width_argument),
      float_argument(mobile_framebuffer_abi::destination_height_argument)};
  const auto valid_rectangle = [](const Rectangle &rectangle) {
    return std::isfinite(rectangle.x) && std::isfinite(rectangle.y) &&
           std::isfinite(rectangle.width) && std::isfinite(rectangle.height) &&
           rectangle.width > 0.0F && rectangle.height > 0.0F;
  };
  if (!valid_rectangle(source) || !valid_rectangle(destination)) {
    call.set_return(iokit_abi::bad_argument);
    return;
  }
  layers_.insert_or_assign(
      layer, LayerState{*identifier, source, destination,
                        call.argument(mobile_framebuffer_abi::flags_argument)});
  call.set_return(iokit_abi::success);
}

void MobileFramebufferHle::submit_layers(UserlandHleCall &call) {
  if (display_ == nullptr)
    return;
  if (submit_host_layers())
    return;
  const auto geometry = display_->geometry();
  std::vector<std::uint32_t> composed(
      geometry.pixel_count(), background_argb_);
  const auto blend_channel = [](std::uint32_t source, std::uint32_t destination,
                                std::uint32_t inverse_alpha) {
    return std::min(255U, source + (destination * inverse_alpha + 127U) / 255U);
  };
  for (const auto &[layer, state] : layers_) {
    static_cast<void>(layer);
    const auto backing = surface_store_->find(state.surface_id);
    auto source_pixels =
        surface_store_->read_argb(call.memory(), state.surface_id);
    if (!backing || !source_pixels || backing->width == 0 ||
        backing->height == 0) {
      continue;
    }
    const auto clipped_edge = [](double value, std::uint32_t maximum) {
      return static_cast<int>(std::clamp(std::floor(value + 0.5), 0.0,
                                         static_cast<double>(maximum)));
    };
    const auto destination_left =
        clipped_edge(state.destination.x, geometry.width);
    const auto destination_top =
        clipped_edge(state.destination.y, geometry.height);
    const auto destination_right = clipped_edge(
        static_cast<double>(state.destination.x) + state.destination.width,
        geometry.width);
    const auto destination_bottom = clipped_edge(
        static_cast<double>(state.destination.y) + state.destination.height,
        geometry.height);
    if (destination_right <= destination_left ||
        destination_bottom <= destination_top) {
      continue;
    }
    const auto full_surface_copy =
        destination_left == 0 && destination_top == 0 &&
        destination_right == static_cast<int>(geometry.width) &&
        destination_bottom == static_cast<int>(geometry.height) &&
        state.destination.x == 0.0F && state.destination.y == 0.0F &&
        state.destination.width ==
            static_cast<float>(geometry.width) &&
        state.destination.height ==
            static_cast<float>(geometry.height) &&
        state.source.x == 0.0F && state.source.y == 0.0F &&
        state.source.width == static_cast<float>(backing->width) &&
        state.source.height == static_cast<float>(backing->height) &&
        backing->width == geometry.width &&
        backing->height == geometry.height &&
        std::all_of(source_pixels->begin(), source_pixels->end(),
                    [](std::uint32_t pixel) { return (pixel >> 24U) == 255U; });
    if (full_surface_copy) {
      composed = std::move(*source_pixels);
      continue;
    }
    std::vector<std::uint32_t> source_columns(
        static_cast<std::size_t>(destination_right - destination_left));
    for (auto x = destination_left; x < destination_right; ++x) {
      const auto horizontal =
          (static_cast<float>(x) + 0.5F - state.destination.x) /
          state.destination.width;
      source_columns[static_cast<std::size_t>(x - destination_left)] =
          static_cast<std::uint32_t>(std::floor(std::clamp(
              static_cast<double>(state.source.x) +
                  horizontal * state.source.width,
              0.0, static_cast<double>(backing->width - 1U))));
    }
    for (auto y = destination_top; y < destination_bottom; ++y) {
      const auto vertical =
          (static_cast<float>(y) + 0.5F - state.destination.y) /
          state.destination.height;
      const auto source_y = static_cast<std::uint32_t>(std::floor(std::clamp(
          static_cast<double>(state.source.y) + vertical * state.source.height,
          0.0, static_cast<double>(backing->height - 1U))));
      for (auto x = destination_left; x < destination_right; ++x) {
        const auto source_x = source_columns[static_cast<std::size_t>(
            x - destination_left)];
        const auto source_pixel =
            (*source_pixels)[static_cast<std::size_t>(source_y) *
                                 backing->width +
                             static_cast<std::size_t>(source_x)];
        auto &destination_pixel =
            composed[static_cast<std::size_t>(y) * geometry.width +
                     static_cast<std::size_t>(x)];
        const auto alpha = source_pixel >> 24U;
        if (alpha == 255U) {
          destination_pixel = source_pixel;
          continue;
        }
        const auto inverse_alpha = 255U - alpha;
        const auto output_alpha =
            blend_channel(alpha, destination_pixel >> 24U, inverse_alpha);
        const auto output_red =
            blend_channel((source_pixel >> 16U) & 0xffU,
                          (destination_pixel >> 16U) & 0xffU, inverse_alpha);
        const auto output_green =
            blend_channel((source_pixel >> 8U) & 0xffU,
                          (destination_pixel >> 8U) & 0xffU, inverse_alpha);
        const auto output_blue = blend_channel(
            source_pixel & 0xffU, destination_pixel & 0xffU, inverse_alpha);
        destination_pixel = (output_alpha << 24U) | (output_red << 16U) |
                            (output_green << 8U) | output_blue;
      }
    }
  }
  display_->replace_pixels(std::move(composed));
}

void MobileFramebufferHle::record_presentation(UserlandHleCall &call) {
  if (!presentation_tracker_)
    return;
  std::vector<PresentationLayer> presented_layers;
  presented_layers.reserve(layers_.size());
  for (const auto &[order, state] : layers_) {
    const auto backing = surface_store_->find(state.surface_id);
    if (!backing)
      continue;
    const auto scale_x = state.source.width / state.destination.width;
    const auto scale_y = state.source.height / state.destination.height;
    presented_layers.push_back(PresentationLayer{
        order,
        state.surface_id,
        backing->provenance,
        PresentationRectangle{state.source.x, state.source.y,
                              state.source.width, state.source.height},
        PresentationRectangle{state.destination.x, state.destination.y,
                              state.destination.width,
                              state.destination.height},
        PresentationTransform{
            scale_x, 0.0F, 0.0F, scale_y,
            state.source.x - state.destination.x * scale_x,
            state.source.y - state.destination.y * scale_y},
        state.flags});
  }
  auto logical_client_scene = scene_coordinator_
                                  ? scene_coordinator_->active_client_scene()
                                  : std::nullopt;
  static_cast<void>(presentation_tracker_->record(
      call.process_id(), std::move(presented_layers),
      std::move(logical_client_scene)));
}

void MobileFramebufferHle::set_background_color(UserlandHleCall &call) {
  const auto channel = [](std::uint32_t bits) {
    const auto value = std::clamp(std::bit_cast<float>(bits), 0.0F, 1.0F);
    return static_cast<std::uint32_t>(std::lround(value * 255.0F));
  };
  const auto red = channel(call.argument(1));
  const auto green = channel(call.argument(2));
  const auto blue = channel(call.argument(3));
  const auto alpha = channel(call.argument(4));
  background_argb_ = (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
  if (display_ && display_write_allowed(call))
    display_->clear(background_argb_);
  call.set_return(iokit_abi::success);
}

} // namespace ilemu
