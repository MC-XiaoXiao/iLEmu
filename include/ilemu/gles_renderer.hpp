#pragma once

#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ilemu/gles_rasterizer.hpp"
#include "ilemu/host_graphics.hpp"
#include "ilemu/performance.hpp"

namespace ilemu {

struct DisplayFrame;

enum class GlesBackend : std::uint8_t {
    Auto,
    Software,
    Vulkan,
};

using GlesRenderTargetKey = HostSurfaceKey;

// Host-side renderer boundary for the guest GLES state model. Backends consume
// already decoded GLES vertices and fixed-function state; they do not depend on
// EGL, CoreSurface, LayerKit, or any guest process details.
class GlesRenderer : public HostGraphicsDevice {
  public:
    virtual ~GlesRenderer() = default;

    [[nodiscard]] std::shared_ptr<HostSurface>
    create_surface(HostSurfaceKey key, HostSurfaceDescriptor descriptor,
                   std::span<const std::uint32_t> initial_pixels = {})
        override;
    [[nodiscard]] std::unique_ptr<CommandEncoder>
    create_command_encoder() override;
    [[nodiscard]] bool
    map_cpu(HostSurface& surface, bool read,
            PerfCpuMapReason reason = PerfCpuMapReason::GpuReadback) override;
    [[nodiscard]] HostNativeImage
    native_image(const HostSurface& surface) const override;
    [[nodiscard]] PresentResult
    present(const std::shared_ptr<HostSurface>& surface) override;
    [[nodiscard]] bool native_presentation_available() const override;

    [[nodiscard]] virtual bool draw(DisplayFrame& frame,
                                    GlesRenderTargetKey target,
                                    std::span<const GlesRasterVertex> vertices,
                                    std::uint32_t mode,
                                    const GlesRasterState& state) = 0;
    // Makes recorded work visible to the host queue without waiting for it.
    [[nodiscard]] virtual bool flush(GlesRenderTargetKey target) = 0;
    // Waits for recorded work without materializing the target on the CPU.
    [[nodiscard]] virtual bool finish(GlesRenderTargetKey target) = 0;
    // Materializes GPU-resident draws in a host-endian CPU frame at an
    // explicit CPU-map/readback boundary. An immediately visible backend may
    // implement this as a no-op.
    [[nodiscard]] virtual bool synchronize(DisplayFrame& frame,
                                           GlesRenderTargetKey target) = 0;
    // Marks a CPU-side clear or another writer as authoritative for the target.
    virtual void invalidate(GlesRenderTargetKey target) = 0;
    virtual void release(GlesRenderTargetKey target) = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual bool accelerated() const = 0;
    [[nodiscard]] virtual bool software_fallback_allowed() const = 0;
    [[nodiscard]] virtual PerfFallbackReason failure_reason() const = 0;
};

struct VulkanPresenterConfiguration {
    std::vector<std::string> instance_extensions;
    std::function<std::uintptr_t(std::uintptr_t)> create_surface;
    std::function<std::pair<std::uint32_t, std::uint32_t>()> drawable_size;
};

// A renderer owns host-wide Vulkan device/queue state and is shared by all
// guest processes. Per-process EGL/GLES resources remain in OpenGlesHle.
// Configure the host policy before the first renderer is requested.
void configure_gles_backend(GlesBackend backend);
// The command-line host points this at its writable data partition before the
// first renderer is created. Empty disables persistent Vulkan pipeline data.
void configure_gles_pipeline_cache(std::filesystem::path path);
// Configures an optional host window before Vulkan instance/device creation.
// The callback boundary keeps SDL and native-window types out of the renderer.
void configure_gles_vulkan_presenter(
    VulkanPresenterConfiguration configuration);
[[nodiscard]] std::string_view gles_backend_name(GlesBackend backend);
[[nodiscard]] std::shared_ptr<GlesRenderer> shared_gles_renderer();
// Releases a shared CoreSurface target only when the renderer is still alive;
// unlike shared_gles_renderer(), this never creates a backend during teardown.
void release_gles_render_target(GlesRenderTargetKey target);
// Release the host renderer while its graphics driver is still initialized.
// The command-line host calls this after all guest runtimes have unwound.
void shutdown_gles_renderer();

} // namespace ilemu
