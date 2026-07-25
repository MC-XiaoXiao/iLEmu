#pragma once

#include <compare>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "ilemu/gles_rasterizer.hpp"

namespace ilemu {

struct DisplayFrame;

struct GlesRenderTargetKey {
    std::uintptr_t owner{};
    std::uint32_t surface{};

    auto operator<=>(const GlesRenderTargetKey&) const = default;
};

// Host-side renderer boundary for the guest GLES state model. Backends consume
// already decoded GLES vertices and fixed-function state; they do not depend on
// EGL, CoreSurface, LayerKit, or any guest process details.
class GlesRenderer {
  public:
    virtual ~GlesRenderer() = default;

    [[nodiscard]] virtual bool draw(DisplayFrame& frame,
                                    GlesRenderTargetKey target,
                                    std::span<const GlesRasterVertex> vertices,
                                    std::uint32_t mode,
                                    const GlesRasterState& state) = 0;
    // Makes any GPU-resident draws visible in the host-endian CPU frame at a
    // guest flush/finish/surface boundary. An immediately visible backend may
    // implement this as a no-op.
    [[nodiscard]] virtual bool synchronize(DisplayFrame& frame,
                                           GlesRenderTargetKey target) = 0;
    // Marks a CPU-side clear or another writer as authoritative for the target.
    virtual void invalidate(GlesRenderTargetKey target) = 0;
    virtual void release(GlesRenderTargetKey target) = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual bool accelerated() const = 0;
};

// A renderer owns host-wide Vulkan device/queue state and is shared by all
// guest processes. Per-process EGL/GLES resources remain in OpenGlesHle.
[[nodiscard]] std::shared_ptr<GlesRenderer> shared_gles_renderer();

} // namespace ilemu
