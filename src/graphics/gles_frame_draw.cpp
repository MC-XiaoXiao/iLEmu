// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "graphics/gles_frame_draw.hpp"

#include "graphics/display.hpp"
#include "graphics/gles_program_renderer.hpp"
#include "graphics/gles_resources.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace ilemu {
namespace {
    constexpr std::size_t payload_limit = 64U * 1024U * 1024U;
    auto textures(const GlesRasterState& raster)
    {
        std::array<std::uint32_t, gles_abi::texture_unit_count> names { };
        for (std::size_t index = 0; index < names.size(); ++index)
            if (raster.texture_units[index].enabled)
                names[index] = raster.texture_units[index].texture;
        return names;
    }
}

struct GlesFrameDraw::State {
    // Credit outlives every owned input container.
    std::shared_ptr<void> credit;
    std::shared_ptr<void> native_lifetime;
    DisplayFrame frame;
    HostSurfaceKey target;
    std::vector<GlesRasterVertex> vertices;
    GlesResourceStore textures;
    GlesRasterState raster;
    std::uint32_t mode { };
    std::size_t result_bytes { };
};

GlesFrameDraw::GlesFrameDraw(std::shared_ptr<State> state)
    : state_ { std::move(state) }
{
}

bool GlesFrameDraw::can_snapshot(
    const DisplayFrame& frame, const GlesRasterState& raster)
{
    if (frame.read_pixels || !frame.width ||
        !frame.height ||
        (frame.host_surface
                ? !frame.pixels.empty()
                : frame.pixels.size() !=
                      static_cast<std::uint64_t>(frame.width) * frame.height))
        return false;
    if (!raster.resources)
        return true;
    for (const auto name : textures(raster)) {
        const auto* texture = raster.resources->texture(name);
        if (!texture)
            continue;
        for (const auto& [level, image] : texture->levels) {
            static_cast<void>(level);
            if (image.host_surface || image.surface_id)
                return false;
        }
    }
    return true;
}

std::shared_ptr<GlesFrameDraw> GlesFrameDraw::prepare(const DisplayFrame& frame,
    HostSurfaceKey target, std::span<const GlesRasterVertex> vertices,
    std::uint32_t mode, const GlesRasterState& raster, std::size_t& bytes,
    std::shared_ptr<void> native_lifetime)
{
    if (!can_snapshot(frame, raster) ||
        (frame.host_surface &&
            (!native_lifetime || frame.host_surface->key() != target)))
        throw std::invalid_argument {
            "GLES draw requires live surface ordering"
        };
    bytes = sizeof(State);
    const auto add = [&](std::size_t size, std::size_t count = 1) {
        if (size && count > (payload_limit - bytes) / size)
            throw std::length_error { "GLES frame draw exceeds 64 MiB" };
        bytes += size * count;
    };
    add(sizeof(std::uint32_t), frame.pixels.size());
    add(sizeof(GlesRasterVertex), vertices.size());
    const auto names = textures(raster);
    if (raster.resources)
        add(1, raster.resources->texture_snapshot_bytes(names, payload_limit));
    auto credit = reserve_gles_host_preparation(bytes);
    if (!credit)
        return { };
    auto state = std::make_shared<State>();
    state->credit = std::move(credit);
    state->native_lifetime = std::move(native_lifetime);
    // No reader, presentation lease or guest-owned view crosses the queue.
    // An isolated native image retains its exact physical owner instead.
    state->frame = DisplayFrame { frame.width, frame.height, 0, frame.pixels,
        frame.host_surface };
    state->target = target;
    state->vertices.assign(vertices.begin(), vertices.end());
    if (raster.resources)
        state->textures = raster.resources->snapshot_textures(names);
    state->raster = raster;
    state->raster.resources = &state->textures;
    state->mode = mode;
    state->result_bytes = frame.pixels.size() * sizeof(std::uint32_t);
    return std::shared_ptr<GlesFrameDraw> { new GlesFrameDraw {
        std::move(state) } };
}

bool GlesFrameDraw::capacity() const
{
    if (result_)
        return true;
    const auto writer = state_->frame.host_surface
                            ? state_->frame.host_surface->writer() : nullptr;
    return (!writer || writer->ready()) &&
           gles_host_work_capacity(state_->result_bytes);
}

std::shared_ptr<GlesWorkResult> GlesFrameDraw::enqueue(
    std::shared_ptr<GlesRenderer> renderer)
{
    if (result_)
        return result_;
    if (!renderer)
        throw std::invalid_argument { "GLES frame renderer is unavailable" };
    auto execute =
        [state = state_, renderer = std::move(renderer)](DisplayFrame& frame) {
            if (state->frame.host_surface) {
                // This path has no software fallback or CPU output. Publish
                // actual native writes even if the guest observer exits; only
                // host-owned surface metadata is touched by this worker.
                if (!renderer->accelerated() ||
                    renderer->software_fallback_allowed() ||
                    !renderer->draw(state->frame, state->target, state->vertices,
                        state->mode, state->raster))
                    return false;
                static_cast<void>(state->frame.host_surface->mark_gpu_write());
                return true;
            }
            frame = std::move(state->frame);
            renderer->invalidate(state->target);
            return renderer->draw(frame, state->target, state->vertices,
                       state->mode, state->raster) &&
                   renderer->synchronize(frame, state->target);
        };
    result_ = state_->frame.host_surface
                  ? enqueue_gles_surface_write(
                        state_->frame.host_surface, std::move(execute))
                  : enqueue_gles_host_work(
                        state_->result_bytes, std::move(execute));
    return result_;
}

bool GlesFrameDraw::complete()
{
    if (!result_ || !result_->ready())
        return false;
    // Result publication follows the worker's last access to this field.
    // The store still owns the image; retirement itself remains worker-owned.
    state_->native_lifetime.reset();
    return result_->succeeded();
}

} // namespace ilemu
