#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <span>

#include "ilemu/gles_rasterizer.hpp"
#include "ilemu/gles_renderer.hpp"
#include "ilemu/gles_resources.hpp"

namespace ilemu {

// Tracks retained scanout pages used by legacy compositors. A compositor may
// build a translucent scene in a local render target and source-over it onto
// alternating scanout surfaces. The physical driver preserves the compositor
// background without carrying the previous transition result into the next
// page; model that boundary independently of the host rendering backend.
class ScanoutComposition {
public:
    void reset();

    void begin_draw(GlesRenderTargetKey key,
        const std::shared_ptr<HostSurface>& surface, std::uint32_t screen_width,
        std::uint32_t screen_height);

    [[nodiscard]] bool restore_background(std::uint32_t process_id,
        GlesRenderTargetKey key, const std::shared_ptr<HostSurface>& surface,
        std::uint32_t screen_width, std::uint32_t screen_height,
        const GlesRasterState& state, const GlesResourceStore& resources,
        std::span<const GlesRasterVertex> vertices, CommandEncoder& encoder,
        bool& restored);

    [[nodiscard]] bool is_background_draw(
        const std::shared_ptr<HostSurface>& surface, std::uint32_t screen_width,
        std::uint32_t screen_height, const GlesRasterState& state,
        std::span<const GlesRasterVertex> vertices) const;

    [[nodiscard]] bool capture_background(std::uint32_t process_id,
        std::uint64_t renderer_owner, GlesRenderTargetKey key,
        const std::shared_ptr<HostSurface>& surface, GlesRenderer& renderer,
        CommandEncoder& encoder);

private:
    struct FrameState {
        std::shared_ptr<HostSurface> surface;
        std::uint64_t observed_presentation { };
        bool active { };
        bool scene_composited { };
    };

    struct BackgroundState {
        std::shared_ptr<HostSurface> surface;
        bool valid { };
    };

    [[nodiscard]] static bool is_screen_surface(
        const std::shared_ptr<HostSurface>& surface, std::uint32_t screen_width,
        std::uint32_t screen_height);
    [[nodiscard]] static bool copy_surface(
        const std::shared_ptr<HostSurface>& source,
        const std::shared_ptr<HostSurface>& destination, HostRectangle region,
        CommandEncoder& encoder);

    std::map<GlesRenderTargetKey, FrameState> frames_;
    std::map<std::uint32_t, BackgroundState> backgrounds_;
};

} // namespace ilemu
