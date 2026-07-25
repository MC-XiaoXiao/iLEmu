#include "ilemu/mbx2d_hle.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ilemu/address_space.hpp"
#include "ilemu/display.hpp"
#include "ilemu/mbx2d_abi.hpp"
#include "ilemu/output.hpp"
#include "ilemu/presentation_tracker.hpp"
#include "ilemu/userland_hle.hpp"

namespace ilemu {
namespace {

constexpr std::string_view mbx2d_image{
    "/System/Library/Frameworks/MBX2D.framework/MBX2D"};
constexpr std::size_t maximum_deferred_traces = 64;
constexpr std::uint32_t mbx_success = mbx2d_abi::success;
constexpr std::uint32_t mbx_failure = mbx2d_abi::failure;
constexpr std::uint32_t bytes_per_pixel = 4;
constexpr std::uint64_t maximum_transformed_pixels = 16U * 1024U * 1024U;

std::int64_t signed_argument(UserlandHleCall &call, std::size_t index) {
  return static_cast<std::int32_t>(call.argument(index));
}

} // namespace

Mbx2dHle::Mbx2dHle(UserlandHleRegistry &registry,
                   std::shared_ptr<DisplayState> display,
                   std::shared_ptr<SurfaceStore> surfaces,
                   std::shared_ptr<PresentationTracker> presentations)
    : display_{std::move(display)},
      surface_store_{surfaces ? std::move(surfaces)
                              : std::make_shared<SurfaceStore>()},
      presentation_tracker_{std::move(presentations)} {
  const auto add = [&](std::string symbol,
                       UserlandHleRegistry::Handler handler) {
    registry.register_function(std::string{mbx2d_image}, std::move(symbol),
                               std::move(handler));
  };
  add("_mbx2DInitialize", [this](UserlandHleCall &call) {
    initialized_ = true;
    call.set_return(mbx_success);
  });
  add("_mbx2DTerminate", [this](UserlandHleCall &call) { terminate(call); });
  add("_mbx2DCtxInitialize",
      [this](UserlandHleCall &call) { call.set_return(allocate_context()); });
  add("_mbx2DCtxTerminate", [this](UserlandHleCall &call) {
    call.set_return(contexts_.erase(call.argument(0)) != 0 ? mbx_success
                                                           : mbx_failure);
  });
  add("_mbx2DCreateSurface", [this](UserlandHleCall &call) {
    call.set_return(allocate_surface(call.argument(0)));
  });
  add("_mbx2DAddClientSurface", [this](UserlandHleCall &call) {
    const auto surface = allocate_client_surface(
        call.argument(0), call.argument(1), call.argument(3));
    call.set_return(surface != 0 && call.write32(call.argument(2), surface)
                        ? mbx_success
                        : mbx_failure);
  });
  add("_mbx2DGetFramebufferSurface", [this](UserlandHleCall &call) {
    if (framebuffer_surface_ == 0) {
      framebuffer_surface_ = allocate_surface(0, true);
    }
    call.set_return(call.write32(call.argument(0), framebuffer_surface_)
                        ? mbx_success
                        : mbx_failure);
  });
  const auto release = [this](UserlandHleCall &call) {
    const auto surface = call.argument(0);
    surfaces_.erase(surface);
    initialized_destinations_.erase(surface);
    destination_frame_sequences_.erase(surface);
    destination_scene_damage_.erase(surface);
    destination_scene_sources_.erase(surface);
    const auto unbind = [surface](RenderState &state) {
      if (state.source && state.source->surface == surface) {
        state.source.reset();
      }
      if (state.destination && state.destination->surface == surface) {
        state.destination.reset();
      }
    };
    unbind(state_);
    for (auto &[handle, state] : contexts_) {
      static_cast<void>(handle);
      unbind(state);
    }
    call.set_return(mbx_success);
  };
  add("_mbx2DReleaseSurface", release);
  add("_mbx2DRemoveClientSurface", release);

  add("_mbx2DSetSourceSurface",
      [this](UserlandHleCall &call) { bind_surface(call, true, false); });
  add("_mbx2DSetDestinationSurface",
      [this](UserlandHleCall &call) { bind_surface(call, false, false); });
  add("_mbx2DCtxSetSourceSurface",
      [this](UserlandHleCall &call) { bind_surface(call, true, true); });
  add("_mbx2DCtxSetDestinationSurface",
      [this](UserlandHleCall &call) { bind_surface(call, false, true); });
  add("_mbx2DSetScissor",
      [this](UserlandHleCall &call) { set_scissor(call, false); });
  add("_mbx2DCtxSetScissor",
      [this](UserlandHleCall &call) { set_scissor(call, true); });
  add("_mbx2DSetBlendEquation", [this](UserlandHleCall &call) {
    set_blend_equation(call, false, false);
  });
  add("_mbx2DCtxSetBlendEquation",
      [this](UserlandHleCall &call) { set_blend_equation(call, true, false); });
  add("_mbx2DSetBlendEquationComplex",
      [this](UserlandHleCall &call) { set_blend_equation(call, false, true); });
  add("_mbx2DCtxSetBlendEquationComplex",
      [this](UserlandHleCall &call) { set_blend_equation(call, true, true); });
  add("_mbx2DSetScaleFactor",
      [this](UserlandHleCall &call) { set_scale_factor(call, false); });
  add("_mbx2DCtxSetScaleFactor",
      [this](UserlandHleCall &call) { set_scale_factor(call, true); });
  add("_mbx2DSetRotation",
      [this](UserlandHleCall &call) { set_rotation(call, false); });
  add("_mbx2DCtxSetRotation",
      [this](UserlandHleCall &call) { set_rotation(call, true); });
  add("_mbx2DEnable",
      [this](UserlandHleCall &call) { set_feature(call, false, true); });
  add("_mbx2DCtxEnable",
      [this](UserlandHleCall &call) { set_feature(call, true, true); });
  add("_mbx2DDisable",
      [this](UserlandHleCall &call) { set_feature(call, false, false); });
  add("_mbx2DCtxDisable",
      [this](UserlandHleCall &call) { set_feature(call, true, false); });
  add("_mbx2DBlitColor",
      [this](UserlandHleCall &call) { blit_color(call, false); });
  add("_mbx2DCtxBlitColor",
      [this](UserlandHleCall &call) { blit_color(call, true); });
  add("_mbx2DBlitCopy",
      [this](UserlandHleCall &call) { blit_copy(call, false); });
  add("_mbx2DCtxBlitCopy",
      [this](UserlandHleCall &call) { blit_copy(call, true); });
  add("_mbx3DQuadColor", [this](UserlandHleCall &call) { quad_color(call); });
  add("_mbx3DQuadCopy", [this](UserlandHleCall &call) { quad_copy(call); });

  const auto finish = [this](UserlandHleCall &call) {
    submit_destination(call, false);
    call.set_return(mbx_success);
  };
  add("_mbx2DFinish", finish);
  add("_mbx2DFlush", finish);
  add("_mbx2DFlushSurfaces",
      [this](UserlandHleCall &call) { flush_surfaces(call); });
  add("_mbx2DFlushInvalidateSurfaces",
      [this](UserlandHleCall &call) { flush_surfaces(call); });
  add("_mbx2DCtxFlush", [this](UserlandHleCall &call) {
    submit_destination(call, true);
    call.set_return(mbx_success);
  });
  const auto present = [this](UserlandHleCall &call) {
    submit_destination(call, false);
    if (display_ &&
        (!presentation_tracker_ ||
         !presentation_tracker_->has_presented_frame()))
      display_->present();
    call.set_return(mbx_success);
  };
  add("_mbx2DSwapSurface", present);
  add("_mbx2DSwapNotification", [](UserlandHleCall &call) {
    // This is the completion callback attached to an enclosing
    // IOMobileFramebuffer transaction. SwapEnd owns presentation after
    // all layer surfaces have been installed.
    call.set_return(mbx_success);
  });

  registry.register_prefix(std::string{mbx2d_image}, "_mbx2D",
                           [this](UserlandHleCall &call) { deferred(call); });
  registry.register_prefix(std::string{mbx2d_image}, "_mbx3D",
                           [this](UserlandHleCall &call) { deferred(call); });
  registry.register_prefix(std::string{mbx2d_image}, "_mbxYUV",
                           [this](UserlandHleCall &call) { deferred(call); });
}

void Mbx2dHle::reset() {
  contexts_.clear();
  next_context_ = first_context_handle;
  surfaces_.clear();
  initialized_destinations_.clear();
  destination_frame_sequences_.clear();
  destination_scene_damage_.clear();
  destination_scene_sources_.clear();
  next_surface_ = first_surface_handle;
  framebuffer_surface_ = 0;
  state_ = {};
  initialized_ = false;
  deferred_trace_count_ = 0;
}

void Mbx2dHle::inherit_state(const Mbx2dHle &parent) {
  contexts_ = parent.contexts_;
  next_context_ = parent.next_context_;
  surfaces_ = parent.surfaces_;
  initialized_destinations_ = parent.initialized_destinations_;
  destination_frame_sequences_ = parent.destination_frame_sequences_;
  destination_scene_damage_ = parent.destination_scene_damage_;
  destination_scene_sources_ = parent.destination_scene_sources_;
  next_surface_ = parent.next_surface_;
  framebuffer_surface_ = parent.framebuffer_surface_;
  state_ = parent.state_;
  initialized_ = parent.initialized_;
  deferred_trace_count_ = parent.deferred_trace_count_;
  presentation_tracker_ = parent.presentation_tracker_;
  surface_store_->inherit_state(*parent.surface_store_);
}

void Mbx2dHle::set_display(std::shared_ptr<DisplayState> display) {
  display_ = std::move(display);
}

std::uint32_t Mbx2dHle::allocate_surface(std::uint32_t core_surface_id,
                                         bool framebuffer) {
  const auto handle = next_surface_++;
  surfaces_.emplace(
      handle, Surface{handle, core_surface_id, framebuffer, std::nullopt});
  return handle;
}

std::uint32_t Mbx2dHle::allocate_context() {
  const auto handle = next_context_++;
  contexts_.emplace(handle, RenderState{});
  return handle;
}

Mbx2dHle::RenderState *Mbx2dHle::select_state(UserlandHleCall &call,
                                              bool context_api) {
  if (!context_api)
    return &state_;
  const auto context = contexts_.find(call.argument(0));
  return context == contexts_.end() ? nullptr : &context->second;
}

void Mbx2dHle::bind_surface(UserlandHleCall &call, bool source,
                            bool context_api) {
  auto *state = select_state(call, context_api);
  if (state == nullptr) {
    call.set_return(mbx_failure);
    return;
  }
  const auto first = context_api ? 1U : 0U;
  const auto handle = call.argument(first);
  auto &binding = source ? state->source : state->destination;
  if (handle == 0) {
    binding.reset();
    call.set_return(mbx_success);
    return;
  }
  if (!surfaces_.contains(handle)) {
    call.set_return(mbx_failure);
    return;
  }
  binding = Binding{handle, call.argument(first + 1U),
                    call.argument(first + 2U), call.argument(first + 3U)};
  if (!source)
    initialize_destination(call, *state);
  call.set_return(mbx_success);
}

void Mbx2dHle::set_scissor(UserlandHleCall &call, bool context_api) {
  auto *state = select_state(call, context_api);
  if (state == nullptr) {
    call.set_return(mbx_failure);
    return;
  }
  const auto first = context_api ? 1U : 0U;
  state->scissor =
      Scissor{static_cast<std::int32_t>(call.argument(first)),
              static_cast<std::int32_t>(call.argument(first + 1U)),
              static_cast<std::int32_t>(call.argument(first + 2U)),
              static_cast<std::int32_t>(call.argument(first + 3U)), true};
  call.set_return(mbx_success);
}

void Mbx2dHle::set_blend_equation(UserlandHleCall &call, bool context_api,
                                  bool complex) {
  auto *state = select_state(call, context_api);
  if (state == nullptr) {
    call.set_return(mbx_failure);
    return;
  }
  const auto first = context_api ? 1U : 0U;
  const auto source = call.argument(first);
  const auto destination = call.argument(first + 1U);
  if (complex) {
    const auto operation = call.argument(first + 2U);
    const auto alpha = static_cast<std::uint8_t>(call.argument(first + 3U));
    if ((source & mbx2d_abi::complex_source_factor_mask) == 0 ||
        (destination & mbx2d_abi::complex_destination_factor_mask) == 0 ||
        (operation & mbx2d_abi::complex_operation_mask) == 0) {
      call.set_return(mbx_failure);
      return;
    }
    state->blend = BlendState{source, destination, operation, alpha, true};
    call.set_return(mbx_success);
    return;
  }

  const auto source_is_extended =
      (source & mbx2d_abi::simple_source_factor_mask) ==
      mbx2d_abi::simple_source_factor_mask;
  const auto destination_is_extended =
      (destination & mbx2d_abi::simple_destination_factor_mask) ==
      mbx2d_abi::simple_destination_factor_mask;
  if (source_is_extended != destination_is_extended) {
    call.set_return(mbx_failure);
    return;
  }
  state->blend =
      BlendState{source, destination, 0,
                 static_cast<std::uint8_t>(call.argument(first + 2U)), false};
  call.set_return(mbx_success);
}

void Mbx2dHle::set_scale_factor(UserlandHleCall &call, bool context_api) {
  auto *state = select_state(call, context_api);
  if (state == nullptr) {
    call.set_return(mbx_failure);
    return;
  }
  const auto first = context_api ? 1U : 0U;
  state->scale_x_bits = call.argument(first);
  state->scale_y_bits = call.argument(first + 1U);
  call.set_return(mbx_success);
}

void Mbx2dHle::set_rotation(UserlandHleCall &call, bool context_api) {
  auto *state = select_state(call, context_api);
  if (state == nullptr) {
    call.set_return(mbx_failure);
    return;
  }
  state->rotation = call.argument(context_api ? 1U : 0U);
  call.set_return(mbx_success);
}

void Mbx2dHle::set_feature(UserlandHleCall &call, bool context_api,
                           bool enabled) {
  auto *state = select_state(call, context_api);
  if (state == nullptr) {
    call.set_return(mbx_failure);
    return;
  }
  const auto feature = call.argument(context_api ? 1U : 0U);
  if (feature != mbx2d_abi::feature_blend &&
      feature != mbx2d_abi::feature_rotation) {
    call.set_return(mbx_failure);
    return;
  }
  if (enabled) {
    state->enabled_features.insert(feature);
  } else {
    state->enabled_features.erase(feature);
    // LayerKit brackets its affine/quad pass with the rotation feature.
    // The scissor programmed for that pass must not leak into the regular
    // 2D blits which follow (notably the lock-screen bottom bar).
    if (feature == mbx2d_abi::feature_rotation) {
      state->scissor.enabled = false;
    }
  }
  call.set_return(mbx_success);
}

std::optional<Mbx2dHle::ResolvedSurface>
Mbx2dHle::resolve(const std::optional<Binding> &binding) const {
  if (!binding)
    return std::nullopt;
  const auto surface = surfaces_.find(binding->surface);
  if (surface == surfaces_.end())
    return std::nullopt;
  if (surface->second.framebuffer) {
    const auto geometry = display_ ? display_->geometry()
                                   : default_display_geometry;
    return ResolvedSurface{std::nullopt, true, geometry.width,
                           geometry.height};
  }
  if (surface->second.client_backing) {
    auto backing = *surface->second.client_backing;
    const auto pitch = binding->pitch;
    std::uint32_t client_bytes_per_pixel{};
    if (binding->format == mbx2d_abi::pixel_format_bgra) {
      backing.pixel_format = surface_pixel_format_bgra;
      client_bytes_per_pixel = bytes_per_pixel;
    } else if (binding->format == mbx2d_abi::pixel_format_rgb555) {
      backing.pixel_format = surface_pixel_format_rgb555;
      client_bytes_per_pixel = 2U;
    } else {
      return std::nullopt;
    }
    const auto row_bytes = static_cast<std::uint64_t>(backing.width) *
                           client_bytes_per_pixel;
    if (pitch < row_bytes || backing.allocation_size < row_bytes) {
      return std::nullopt;
    }
    backing.bytes_per_row = pitch;
    backing.height = static_cast<std::uint32_t>(
        (backing.allocation_size - row_bytes) / pitch + 1U);
    return ResolvedSurface{backing, false, backing.width, backing.height};
  }
  const auto backing = surface_store_->find(surface->second.core_surface_id);
  if (!backing)
    return std::nullopt;
  return ResolvedSurface{backing, false, backing->width, backing->height};
}

bool Mbx2dHle::clip_region(BlitRegion &region, const ResolvedSurface *source,
                           const ResolvedSurface &destination,
                           const Scissor &scissor) const {
  if (region.width <= 0 || region.height <= 0)
    return false;
  const auto clip_destination_axis =
      [](std::int64_t &destination_position, std::int64_t &source_position,
         std::int64_t &size, std::int64_t lower, std::int64_t upper) {
        if (destination_position < lower) {
          const auto difference = lower - destination_position;
          destination_position += difference;
          source_position += difference;
          size -= difference;
        }
        if (destination_position + size > upper) {
          size = upper - destination_position;
        }
      };
  const auto clip_source_axis = [](std::int64_t &source_position,
                                   std::int64_t &destination_position,
                                   std::int64_t &size, std::int64_t upper) {
    if (source_position < 0) {
      const auto difference = -source_position;
      source_position += difference;
      destination_position += difference;
      size -= difference;
    }
    if (source_position + size > upper) {
      size = upper - source_position;
    }
  };

  std::int64_t left = 0;
  std::int64_t top = 0;
  std::int64_t right = destination.width;
  std::int64_t bottom = destination.height;
  if (scissor.enabled) {
    left = std::max(left, static_cast<std::int64_t>(scissor.left));
    top = std::max(top, static_cast<std::int64_t>(scissor.top));
    right = std::min(right, static_cast<std::int64_t>(scissor.right));
    bottom = std::min(bottom, static_cast<std::int64_t>(scissor.bottom));
  }
  if (right <= left || bottom <= top)
    return false;
  clip_destination_axis(region.destination_x, region.source_x, region.width,
                        left, right);
  clip_destination_axis(region.destination_y, region.source_y, region.height,
                        top, bottom);
  if (source) {
    clip_source_axis(region.source_x, region.destination_x, region.width,
                     source->width);
    clip_source_axis(region.source_y, region.destination_y, region.height,
                     source->height);
    // Source clipping can move the destination; apply destination bounds
    // once more without changing the already-clipped source origin twice.
    clip_destination_axis(region.destination_x, region.source_x, region.width,
                          left, right);
    clip_destination_axis(region.destination_y, region.source_y, region.height,
                          top, bottom);
  }
  return region.width > 0 && region.height > 0;
}

std::optional<std::vector<std::uint32_t>>
Mbx2dHle::read_region(const ResolvedSurface &surface, std::int64_t x,
                      std::int64_t y, std::int64_t width, std::int64_t height,
                      UserlandHleCall &call) const {
  if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
      x + width > surface.width || y + height > surface.height) {
    return std::nullopt;
  }
  std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));
  if (surface.framebuffer) {
    if (!display_)
      return std::nullopt;
    const auto frame = display_->snapshot();
    for (std::int64_t row = 0; row < height; ++row) {
      const auto source = static_cast<std::size_t>((y + row) * frame.width + x);
      const auto destination = static_cast<std::size_t>(row * width);
      std::copy_n(frame.pixels.begin() + source,
                  static_cast<std::size_t>(width),
                  pixels.begin() + destination);
    }
    return pixels;
  }
  if (!surface.backing ||
      (surface.backing->pixel_format != surface_pixel_format_bgra &&
       surface.backing->pixel_format != surface_pixel_format_rgb555)) {
    return std::nullopt;
  }
  const auto &backing = *surface.backing;
  const auto backing_bytes_per_pixel =
      backing.pixel_format == surface_pixel_format_bgra ? bytes_per_pixel : 2U;
  if (backing.bytes_per_row < surface.width * backing_bytes_per_pixel)
    return std::nullopt;
  const auto final_byte =
      static_cast<std::uint64_t>(y + height - 1) * backing.bytes_per_row +
      static_cast<std::uint64_t>(x + width) * backing_bytes_per_pixel;
  if (final_byte > backing.allocation_size)
    return std::nullopt;
  for (std::int64_t row = 0; row < height; ++row) {
    const auto address = backing.base + static_cast<std::uint32_t>(
                                            (y + row) * backing.bytes_per_row +
                                            x * backing_bytes_per_pixel);
    const auto bytes = call.memory().read_bytes(
        address, static_cast<std::size_t>(width) * backing_bytes_per_pixel);
    if (!bytes)
      return std::nullopt;
    if (backing.pixel_format == surface_pixel_format_rgb555) {
      for (std::int64_t column = 0; column < width; ++column) {
        const auto byte = static_cast<std::size_t>(column) * 2U;
        const auto packed =
            std::to_integer<std::uint32_t>((*bytes)[byte]) |
            (std::to_integer<std::uint32_t>((*bytes)[byte + 1U]) << 8U);
        const auto red = ((packed >> 10U) & 0x1fU) * 255U / 31U;
        const auto green = ((packed >> 5U) & 0x1fU) * 255U / 31U;
        const auto blue = (packed & 0x1fU) * 255U / 31U;
        pixels[static_cast<std::size_t>(row * width + column)] =
            0xff000000U | (red << 16U) | (green << 8U) | blue;
      }
      continue;
    }
    if constexpr (std::endian::native == std::endian::little) {
      std::memcpy(pixels.data() + static_cast<std::size_t>(row * width),
                  bytes->data(),
                  static_cast<std::size_t>(width) * bytes_per_pixel);
      continue;
    }
    for (std::int64_t column = 0; column < width; ++column) {
      const auto byte = static_cast<std::size_t>(column) * bytes_per_pixel;
      pixels[static_cast<std::size_t>(row * width + column)] =
          std::to_integer<std::uint32_t>((*bytes)[byte]) |
          (std::to_integer<std::uint32_t>((*bytes)[byte + 1U]) << 8U) |
          (std::to_integer<std::uint32_t>((*bytes)[byte + 2U]) << 16U) |
          (std::to_integer<std::uint32_t>((*bytes)[byte + 3U]) << 24U);
    }
  }
  return pixels;
}

bool Mbx2dHle::write_region(const ResolvedSurface &surface, std::int64_t x,
                            std::int64_t y, std::int64_t width,
                            std::int64_t height,
                            const std::vector<std::uint32_t> &pixels,
                            UserlandHleCall &call) {
  if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
      x + width > surface.width || y + height > surface.height ||
      pixels.size() != static_cast<std::size_t>(width * height)) {
    return false;
  }
  if (surface.framebuffer) {
    if (!display_)
      return false;
    auto frame = display_->snapshot();
    for (std::int64_t row = 0; row < height; ++row) {
      const auto source = static_cast<std::size_t>(row * width);
      const auto destination =
          static_cast<std::size_t>((y + row) * frame.width + x);
      std::copy_n(pixels.begin() + source, static_cast<std::size_t>(width),
                  frame.pixels.begin() + destination);
    }
    display_->replace_pixels(std::move(frame.pixels));
    return true;
  }
  if (!surface.backing ||
      (surface.backing->pixel_format != surface_pixel_format_bgra &&
       surface.backing->pixel_format != surface_pixel_format_rgb555)) {
    return false;
  }
  const auto &backing = *surface.backing;
  const auto backing_bytes_per_pixel =
      backing.pixel_format == surface_pixel_format_bgra ? bytes_per_pixel : 2U;
  if (backing.bytes_per_row < surface.width * backing_bytes_per_pixel)
    return false;
  const auto final_byte =
      static_cast<std::uint64_t>(y + height - 1) * backing.bytes_per_row +
      static_cast<std::uint64_t>(x + width) * backing_bytes_per_pixel;
  if (final_byte > backing.allocation_size)
    return false;
  std::vector<std::byte> encoded(static_cast<std::size_t>(width) *
                                 backing_bytes_per_pixel);
  for (std::int64_t row = 0; row < height; ++row) {
    if (backing.pixel_format == surface_pixel_format_rgb555) {
      for (std::int64_t column = 0; column < width; ++column) {
        const auto pixel =
            pixels[static_cast<std::size_t>(row * width + column)];
        const auto red = ((pixel >> 16U) & 0xffU) * 31U / 255U;
        const auto green = ((pixel >> 8U) & 0xffU) * 31U / 255U;
        const auto blue = (pixel & 0xffU) * 31U / 255U;
        const auto packed = (red << 10U) | (green << 5U) | blue;
        const auto byte = static_cast<std::size_t>(column) * 2U;
        encoded[byte] = static_cast<std::byte>(packed & 0xffU);
        encoded[byte + 1U] = static_cast<std::byte>(packed >> 8U);
      }
    } else if constexpr (std::endian::native == std::endian::little) {
      std::memcpy(encoded.data(),
                  pixels.data() + static_cast<std::size_t>(row * width),
                  encoded.size());
    } else {
      for (std::int64_t column = 0; column < width; ++column) {
        const auto pixel =
            pixels[static_cast<std::size_t>(row * width + column)];
        const auto byte = static_cast<std::size_t>(column) * bytes_per_pixel;
        encoded[byte] = static_cast<std::byte>(pixel & 0xffU);
        encoded[byte + 1U] = static_cast<std::byte>((pixel >> 8U) & 0xffU);
        encoded[byte + 2U] = static_cast<std::byte>((pixel >> 16U) & 0xffU);
        encoded[byte + 3U] = static_cast<std::byte>((pixel >> 24U) & 0xffU);
      }
    }
    const auto address = backing.base + static_cast<std::uint32_t>(
                                            (y + row) * backing.bytes_per_row +
                                            x * backing_bytes_per_pixel);
    if (!call.memory().copy_in(address, encoded))
      return false;
  }
  return true;
}

std::optional<std::vector<std::uint32_t>> Mbx2dHle::composite(
    const RenderState &state, const ResolvedSurface &destination,
    std::int64_t x, std::int64_t y, std::int64_t width, std::int64_t height,
    const std::vector<std::uint32_t> &source, UserlandHleCall &call) const {
  if (!state.enabled_features.contains(mbx2d_abi::feature_blend)) {
    return source;
  }
  auto destination_pixels = read_region(destination, x, y, width, height, call);
  if (!destination_pixels || destination_pixels->size() != source.size()) {
    return std::nullopt;
  }
  const auto scale_byte = [](std::uint32_t value, std::uint32_t factor) {
    return (value * factor + 127U) / 255U;
  };
  const auto alpha = static_cast<std::uint32_t>(state.blend.global_alpha);
  const auto is_constant_alpha_crossfade =
      !state.blend.complex &&
      state.blend.source_factor == mbx2d_abi::layerkit_crossfade_source_word &&
      state.blend.destination_factor ==
          mbx2d_abi::layerkit_crossfade_destination_word;
  const auto is_straight_alpha_source_over =
      state.blend.complex &&
      state.blend.source_factor == mbx2d_abi::layerkit_mask_source_word &&
      state.blend.destination_factor ==
          mbx2d_abi::layerkit_mask_destination_word &&
      state.blend.operation == mbx2d_abi::layerkit_mask_operation_word;
  for (std::size_t index = 0; index < source.size(); ++index) {
    const auto source_pixel = source[index];
    const auto destination_pixel = (*destination_pixels)[index];
    if (is_constant_alpha_crossfade) {
      const auto inverse_alpha = 255U - alpha;
      std::uint32_t result{};
      for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        const auto source_channel =
            scale_byte((source_pixel >> shift) & 0xffU, alpha);
        const auto destination_channel =
            scale_byte((destination_pixel >> shift) & 0xffU, inverse_alpha);
        result |= std::min(255U, source_channel + destination_channel) << shift;
      }
      (*destination_pixels)[index] = result;
      continue;
    }
    const auto source_alpha = scale_byte(source_pixel >> 24U, alpha);
    const auto inverse_alpha = 255U - source_alpha;
    std::uint32_t result{};
    for (std::uint32_t shift = 0; shift < 24U; shift += 8U) {
      const auto source_channel =
          scale_byte((source_pixel >> shift) & 0xffU,
                     is_straight_alpha_source_over ? source_alpha : alpha);
      const auto destination_channel =
          scale_byte((destination_pixel >> shift) & 0xffU, inverse_alpha);
      const auto channel = std::min(255U, source_channel + destination_channel);
      result |= channel << shift;
    }
    const auto destination_alpha =
        scale_byte(destination_pixel >> 24U, inverse_alpha);
    result |= std::min(255U, source_alpha + destination_alpha) << 24U;
    (*destination_pixels)[index] = result;
  }
  return destination_pixels;
}

std::optional<std::vector<std::uint32_t>>
Mbx2dHle::transform_copy(const RenderState &state, std::int64_t source_width,
                         std::int64_t source_height,
                         const std::vector<std::uint32_t> &source,
                         std::int64_t &output_width,
                         std::int64_t &output_height, UserlandHleCall &call) {
  const auto scale_x = std::bit_cast<float>(state.scale_x_bits);
  const auto scale_y = std::bit_cast<float>(state.scale_y_bits);
  if (!std::isfinite(scale_x) || !std::isfinite(scale_y) || scale_x == 0.0F ||
      scale_y == 0.0F || source_width <= 0 || source_height <= 0 ||
      source.size() != static_cast<std::size_t>(source_width * source_height)) {
    return std::nullopt;
  }
  const auto scaled_width_value =
      static_cast<double>(source_width) * std::abs(scale_x);
  const auto scaled_height_value =
      static_cast<double>(source_height) * std::abs(scale_y);
  if (!std::isfinite(scaled_width_value) ||
      !std::isfinite(scaled_height_value) ||
      scaled_width_value > static_cast<double>(maximum_transformed_pixels) ||
      scaled_height_value > static_cast<double>(maximum_transformed_pixels)) {
    return std::nullopt;
  }
  const auto scaled_width =
      static_cast<std::int64_t>(std::llround(scaled_width_value));
  const auto scaled_height =
      static_cast<std::int64_t>(std::llround(scaled_height_value));
  if (scaled_width <= 0 || scaled_height <= 0 ||
      static_cast<std::uint64_t>(scaled_width) >
          maximum_transformed_pixels /
              static_cast<std::uint64_t>(scaled_height)) {
    return std::nullopt;
  }
  std::vector<std::uint32_t> scaled(
      static_cast<std::size_t>(scaled_width * scaled_height));
  for (std::int64_t y = 0; y < scaled_height; ++y) {
    auto source_y = std::min(
        source_height - 1,
        static_cast<std::int64_t>(static_cast<double>(y) / std::abs(scale_y)));
    if (scale_y < 0.0F)
      source_y = source_height - 1 - source_y;
    for (std::int64_t x = 0; x < scaled_width; ++x) {
      auto source_x = std::min(source_width - 1,
                               static_cast<std::int64_t>(
                                   static_cast<double>(x) / std::abs(scale_x)));
      if (scale_x < 0.0F)
        source_x = source_width - 1 - source_x;
      scaled[static_cast<std::size_t>(y * scaled_width + x)] =
          source[static_cast<std::size_t>(source_y * source_width + source_x)];
    }
  }

  auto rotation = mbx2d_abi::rotation_identity;
  if (state.enabled_features.contains(mbx2d_abi::feature_rotation)) {
    rotation = state.rotation;
  }
  if (rotation == mbx2d_abi::rotation_identity) {
    output_width = scaled_width;
    output_height = scaled_height;
    return scaled;
  }
  if (rotation != mbx2d_abi::rotation_clockwise_90 &&
      rotation != mbx2d_abi::rotation_180 &&
      rotation != mbx2d_abi::rotation_clockwise_270) {
    if (deferred_trace_count_ < maximum_deferred_traces) {
      call.output().write(
          "[mbx2d-hle] unsupported rotation=" + std::to_string(rotation) +
          " pid=" + std::to_string(call.process_id()) + "\n");
      ++deferred_trace_count_;
    }
    output_width = scaled_width;
    output_height = scaled_height;
    return scaled;
  }

  const auto quarter_turn = rotation == mbx2d_abi::rotation_clockwise_90 ||
                            rotation == mbx2d_abi::rotation_clockwise_270;
  output_width = quarter_turn ? scaled_height : scaled_width;
  output_height = quarter_turn ? scaled_width : scaled_height;
  std::vector<std::uint32_t> transformed(
      static_cast<std::size_t>(output_width * output_height));
  for (std::int64_t y = 0; y < output_height; ++y) {
    for (std::int64_t x = 0; x < output_width; ++x) {
      std::int64_t source_x{};
      std::int64_t source_y{};
      if (rotation == mbx2d_abi::rotation_clockwise_90) {
        source_x = y;
        source_y = scaled_height - 1 - x;
      } else if (rotation == mbx2d_abi::rotation_180) {
        source_x = scaled_width - 1 - x;
        source_y = scaled_height - 1 - y;
      } else {
        source_x = scaled_width - 1 - y;
        source_y = x;
      }
      transformed[static_cast<std::size_t>(y * output_width + x)] =
          scaled[static_cast<std::size_t>(source_y * scaled_width + source_x)];
    }
  }
  return transformed;
}

void Mbx2dHle::blit_color(UserlandHleCall &call, bool context_api) {
  auto *state = select_state(call, context_api);
  if (state == nullptr) {
    call.set_return(mbx_failure);
    return;
  }
  const auto first = context_api ? 1U : 0U;
  const auto destination = resolve(state->destination);
  if (!destination) {
    call.set_return(mbx_failure);
    return;
  }
  BlitRegion region{0,
                    0,
                    signed_argument(call, first),
                    signed_argument(call, first + 1U),
                    signed_argument(call, first + 2U),
                    signed_argument(call, first + 3U)};
  if (!clip_region(region, nullptr, *destination, state->scissor)) {
    call.set_return(mbx_success);
    return;
  }
  const auto color = call.argument(first + 4U);
  const std::vector<std::uint32_t> source_pixels(
      static_cast<std::size_t>(region.width * region.height), color);
  const auto pixels = composite(*state, *destination, region.destination_x,
                                region.destination_y, region.width,
                                region.height, source_pixels, call);
  call.set_return(pixels && write_region(*destination, region.destination_x,
                                         region.destination_y, region.width,
                                         region.height, *pixels, call)
                      ? mbx_success
                      : mbx_failure);
}

void Mbx2dHle::blit_copy(UserlandHleCall &call, bool context_api) {
  auto *state = select_state(call, context_api);
  if (state == nullptr) {
    call.set_return(mbx_failure);
    return;
  }
  const auto first = context_api ? 1U : 0U;
  const auto source = resolve(state->source);
  const auto destination = resolve(state->destination);
  if (!source || !destination) {
    call.set_return(mbx_failure);
    return;
  }
  BlitRegion region{
      signed_argument(call, first),      signed_argument(call, first + 1U),
      signed_argument(call, first + 2U), signed_argument(call, first + 3U),
      signed_argument(call, first + 4U), signed_argument(call, first + 5U)};
  const auto transform_enabled =
      state->scale_x_bits != mbx2d_abi::float_one_bits ||
      state->scale_y_bits != mbx2d_abi::float_one_bits ||
      (state->enabled_features.contains(mbx2d_abi::feature_rotation) &&
       state->rotation != mbx2d_abi::rotation_identity);
  if (!transform_enabled) {
    if (!clip_region(region, &*source, *destination, state->scissor)) {
      call.set_return(mbx_success);
      return;
    }
    const auto source_pixels =
        read_region(*source, region.source_x, region.source_y, region.width,
                    region.height, call);
    const auto pixels =
        source_pixels ? composite(*state, *destination, region.destination_x,
                                  region.destination_y, region.width,
                                  region.height, *source_pixels, call)
                      : std::nullopt;
    call.set_return(pixels && write_region(*destination, region.destination_x,
                                           region.destination_y, region.width,
                                           region.height, *pixels, call)
                        ? mbx_success
                        : mbx_failure);
    return;
  }

  if (region.source_x < 0 || region.source_y < 0 || region.width <= 0 ||
      region.height <= 0 || region.source_x + region.width > source->width ||
      region.source_y + region.height > source->height) {
    call.set_return(mbx_failure);
    return;
  }
  const auto source_pixels =
      read_region(*source, region.source_x, region.source_y, region.width,
                  region.height, call);
  std::int64_t transformed_width{};
  std::int64_t transformed_height{};
  const auto transformed =
      source_pixels
          ? transform_copy(*state, region.width, region.height, *source_pixels,
                           transformed_width, transformed_height, call)
          : std::nullopt;
  if (!transformed) {
    call.set_return(mbx_failure);
    return;
  }
  BlitRegion transformed_region{0,
                                0,
                                region.destination_x,
                                region.destination_y,
                                transformed_width,
                                transformed_height};
  if (!clip_region(transformed_region, nullptr, *destination, state->scissor)) {
    call.set_return(mbx_success);
    return;
  }
  std::vector<std::uint32_t> clipped(static_cast<std::size_t>(
      transformed_region.width * transformed_region.height));
  for (std::int64_t y = 0; y < transformed_region.height; ++y) {
    const auto source_offset = static_cast<std::size_t>(
        (transformed_region.source_y + y) * transformed_width +
        transformed_region.source_x);
    const auto destination_offset =
        static_cast<std::size_t>(y * transformed_region.width);
    std::copy_n(transformed->begin() + source_offset,
                static_cast<std::size_t>(transformed_region.width),
                clipped.begin() + destination_offset);
  }
  const auto pixels =
      composite(*state, *destination, transformed_region.destination_x,
                transformed_region.destination_y, transformed_region.width,
                transformed_region.height, clipped, call);
  call.set_return(
      pixels && write_region(*destination, transformed_region.destination_x,
                             transformed_region.destination_y,
                             transformed_region.width,
                             transformed_region.height, *pixels, call)
          ? mbx_success
          : mbx_failure);
}

void Mbx2dHle::submit_destination(UserlandHleCall &call, bool context_api) {
  auto *state = select_state(call, context_api);
  if (state == nullptr)
    return;
  const auto destination = resolve(state->destination);
  if (!destination || destination->framebuffer || !display_ ||
      destination->width != display_->width() ||
      destination->height != display_->height()) {
    return;
  }
  // Once firmware has completed a transactional hardware-layer frame,
  // IOMobileFramebuffer SwapEnd owns scanout. MBX finish/swap still commits
  // pixels to its CoreSurface backing, but presenting that individual render
  // target would bypass the other retained layers.
  if (presentation_tracker_ &&
      presentation_tracker_->has_presented_frame()) {
    return;
  }
  const auto pixels = read_region(*destination, 0, 0, destination->width,
                                  destination->height, call);
  if (pixels) {
    display_->replace_pixels(*pixels);
  }
}

void Mbx2dHle::flush_surfaces(UserlandHleCall &call) {
  const auto array = call.argument(0);
  const auto count = call.argument(1);
  if (count == 0) {
    call.set_return(mbx_success);
    return;
  }
  if (array == 0 || count > mbx2d_abi::maximum_flush_surface_count) {
    call.set_return(mbx_failure);
    return;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto address =
        static_cast<std::uint64_t>(array) +
        static_cast<std::uint64_t>(index) * sizeof(std::uint32_t);
    if (address > std::numeric_limits<std::uint32_t>::max()) {
      call.set_return(mbx_failure);
      return;
    }
    const auto surface =
        call.memory().read32(static_cast<std::uint32_t>(address));
    if (!surface || !surfaces_.contains(*surface)) {
      call.set_return(mbx_failure);
      return;
    }
  }
  // Software blits update guest CoreSurface storage synchronously, so the
  // cache clean/invalidate requested from the real GPU has no extra work.
  call.set_return(mbx_success);
}

void Mbx2dHle::terminate(UserlandHleCall &call) {
  if (!initialized_) {
    call.set_return(mbx_failure);
    return;
  }
  reset();
  call.set_return(mbx_success);
}

void Mbx2dHle::deferred(UserlandHleCall &call) {
  if (deferred_trace_count_ < maximum_deferred_traces) {
    call.output().write(
        "[mbx2d-hle] deferred symbol=" + std::string{call.symbol()} +
        " pid=" + std::to_string(call.process_id()) + "\n");
    ++deferred_trace_count_;
  }
  call.set_return(mbx_success);
}

} // namespace ilemu
