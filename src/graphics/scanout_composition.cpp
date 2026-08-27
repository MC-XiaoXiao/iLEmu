#include "ilemu/scanout_composition.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "ilemu/gles_abi.hpp"

namespace ilemu {
namespace {

    constexpr std::uint64_t scanout_background_namespace = 2ULL << 32U;
    constexpr std::uint64_t minimum_scene_height_numerator = 2U;
    constexpr std::uint64_t minimum_scene_height_denominator = 3U;

} // namespace

void ScanoutComposition::reset()
{
    frames_.clear();
    backgrounds_.clear();
}

bool ScanoutComposition::is_screen_surface(
    const std::shared_ptr<HostSurface>& surface, std::uint32_t screen_width,
    std::uint32_t screen_height)
{
    if (!surface)
        return false;
    const auto descriptor = surface->descriptor();
    return descriptor.width == screen_width &&
           descriptor.height == screen_height;
}

bool ScanoutComposition::copy_surface(
    const std::shared_ptr<HostSurface>& source,
    const std::shared_ptr<HostSurface>& destination, CommandEncoder& encoder)
{
    if (!source || !destination)
        return false;
    if (source == destination)
        return true;
    const auto source_descriptor = source->descriptor();
    const auto destination_descriptor = destination->descriptor();
    if (source_descriptor.width != destination_descriptor.width ||
        source_descriptor.height != destination_descriptor.height) {
        return false;
    }
    const auto rectangle = HostRectangle { 0, 0, source_descriptor.width,
        source_descriptor.height };
    return encoder.copy(source, destination, rectangle, rectangle) &&
           encoder.submit(PerfSubmitReason::GlesSync);
}

void ScanoutComposition::begin_draw(GlesRenderTargetKey key,
    const std::shared_ptr<HostSurface>& surface, std::uint32_t screen_width,
    std::uint32_t screen_height)
{
    if (!is_screen_surface(surface, screen_width, screen_height))
        return;

    for (auto& [frame_key, frame] : frames_) {
        static_cast<void>(frame_key);
        if (!frame.surface)
            continue;
        const auto presentation =
            frame.surface->scanout_presentation_sequence();
        if (presentation == frame.observed_presentation)
            continue;
        frame.observed_presentation = presentation;
        frame.active = false;
    }

    auto& frame = frames_[key];
    if (frame.surface != surface) {
        frame = { surface, surface->scanout_presentation_sequence(), false,
            false };
    }
    if (frame.active)
        return;
    frame.active = true;
    frame.scene_composited = false;
}

bool ScanoutComposition::restore_background(std::uint32_t process_id,
    GlesRenderTargetKey key, const std::shared_ptr<HostSurface>& surface,
    std::uint32_t screen_width, std::uint32_t screen_height,
    const GlesRasterState& state, const GlesResourceStore& resources,
    CommandEncoder& encoder, bool& restored)
{
    restored = false;
    if (!is_screen_surface(surface, screen_width, screen_height) ||
        !state.blend_enabled || state.blend_source != gles_abi::one ||
        state.blend_destination != gles_abi::one_minus_source_alpha) {
        return true;
    }

    const auto descriptor = surface->descriptor();
    const auto covering_scene = std::any_of(state.texture_units.begin(),
        state.texture_units.end(), [&](const GlesRasterTextureUnit& unit) {
            if (!unit.enabled)
                return false;
            const auto* texture = resources.texture(unit.texture);
            if (!texture)
                return false;
            const auto level = texture->levels.find(0U);
            return level != texture->levels.end() &&
                   !level->second.surface_id && level->second.host_surface &&
                   level->second.internal_format == gles_abi::rgba &&
                   level->second.width >= descriptor.width &&
                   static_cast<std::uint64_t>(level->second.height) *
                           minimum_scene_height_denominator >=
                       static_cast<std::uint64_t>(descriptor.height) *
                           minimum_scene_height_numerator;
        });
    if (!covering_scene)
        return true;

    const auto frame = frames_.find(key);
    if (frame == frames_.end())
        return true;
    const auto first_scene_draw = !frame->second.scene_composited;
    frame->second.scene_composited = true;
    if (!first_scene_draw)
        return true;

    const auto background = backgrounds_.find(process_id);
    if (background == backgrounds_.end() || !background->second.valid)
        return true;
    if (!copy_surface(background->second.surface, surface, encoder))
        return false;
    restored = true;
    return true;
}

bool ScanoutComposition::is_background_draw(
    const std::shared_ptr<HostSurface>& surface, std::uint32_t screen_width,
    std::uint32_t screen_height, const GlesRasterState& state,
    std::span<const GlesRasterVertex> vertices) const
{
    if (!is_screen_surface(surface, screen_width, screen_height) ||
        vertices.empty() || state.blend_enabled || state.scissor_enabled ||
        state.viewport_x != 0 || state.viewport_y != 0 ||
        state.viewport_width != screen_width ||
        state.viewport_height != screen_height) {
        return false;
    }

    auto minimum_x = std::numeric_limits<float>::infinity();
    auto maximum_x = -std::numeric_limits<float>::infinity();
    auto minimum_y = std::numeric_limits<float>::infinity();
    auto maximum_y = -std::numeric_limits<float>::infinity();
    for (const auto& vertex : vertices) {
        if (!std::isfinite(vertex.position[3]) ||
            std::abs(vertex.position[3]) <= 1.0e-6F) {
            return false;
        }
        const auto inverse_w = 1.0F / vertex.position[3];
        const auto x = vertex.position[0] * inverse_w;
        const auto y = vertex.position[1] * inverse_w;
        minimum_x = std::min(minimum_x, x);
        maximum_x = std::max(maximum_x, x);
        minimum_y = std::min(minimum_y, y);
        maximum_y = std::max(maximum_y, y);
    }
    constexpr auto edge_tolerance = 0.001F;
    return minimum_x <= -1.0F + edge_tolerance &&
           maximum_x >= 1.0F - edge_tolerance &&
           minimum_y <= -1.0F + edge_tolerance &&
           maximum_y >= 1.0F - edge_tolerance;
}

bool ScanoutComposition::capture_background(std::uint32_t process_id,
    std::uint64_t renderer_owner, GlesRenderTargetKey key,
    const std::shared_ptr<HostSurface>& surface, GlesRenderer& renderer,
    CommandEncoder& encoder)
{
    const auto frame = frames_.find(key);
    if (!surface || (frame != frames_.end() && frame->second.scene_composited))
        return true;

    const auto descriptor = surface->descriptor();
    auto& background = backgrounds_[process_id];
    if (!background.surface ||
        background.surface->descriptor().width != descriptor.width ||
        background.surface->descriptor().height != descriptor.height) {
        const auto backing_key = HostSurfaceKey { renderer_owner,
            scanout_background_namespace | process_id };
        background.surface = renderer.create_surface(backing_key, descriptor);
        background.valid = false;
    }
    background.valid = background.surface &&
                       copy_surface(surface, background.surface, encoder);
    return background.valid;
}

} // namespace ilemu
