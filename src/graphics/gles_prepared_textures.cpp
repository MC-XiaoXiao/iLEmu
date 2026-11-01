// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "graphics/gles_prepared_textures.hpp"
#include "graphics/surface_store.hpp"
#include "graphics/gles_work_completion.hpp"

#include <algorithm>
#include <stdexcept>

namespace ilemu {
std::size_t GlesPreparedTextures::payload_bytes(const GlesResourceStore& source,
    std::span<const std::uint32_t> names, std::size_t limit)
{
    auto bytes = source.texture_snapshot_bytes(names, limit);
    const auto add = [&](std::size_t size, std::size_t count = 1) {
        if (count > (limit - bytes) / size)
            throw std::length_error {
                "GLES texture preparation exceeds budget"
            };
        bytes += size * count;
    };
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto preceding = names.first(i);
        if (std::find(preceding.begin(), preceding.end(), names[i]) !=
            preceding.end())
            continue;
        const auto* texture = source.texture(names[i]);
        if (!texture)
            continue;
        for (const auto& [level, image] : texture->levels) {
            static_cast<void>(level);
            if (!image.host_surface)
                continue;
            // Include the transport lease/control block as well as the vector
            // entry.
            add(sizeof(Source) + 8U * sizeof(void*));
            // Account even a currently cached surface: a concurrent native
            // producer can change its generation before the snapshot below.
            // Retained capture plus temporary readback, in addition to argb.
            const auto pixels =
                static_cast<std::uint64_t>(image.width) * image.height;
            if (pixels > limit / (2U * sizeof(std::uint32_t)))
                throw std::length_error {
                    "GLES texture readback exceeds budget"
                };
            add(2U * sizeof(std::uint32_t), static_cast<std::size_t>(pixels));
        }
    }
    return bytes;
}

GlesPreparedTextures::GlesPreparedTextures(const GlesResourceStore& source,
    std::span<const std::uint32_t> names, const SurfaceStore& surfaces)
    : textures_ { source.snapshot_textures(names) }
{
    std::size_t count = 0;
    // Reserve only surface entries, matching the pre-allocation budget.
    for (const auto& [name, texture] : textures_.textures_)
        for (const auto& [level, copied] : texture.levels) {
            static_cast<void>(copied);
            if (source.texture(name)->levels.at(level).host_surface)
                ++count;
        }
    sources_.reserve(count);
    for (const auto& [name, texture] : textures_.textures_)
        for (const auto& [level, copied] : texture.levels) {
            static_cast<void>(copied);
            const auto& image = source.texture(name)->levels.at(level);
            if (!image.host_surface)
                continue;
            const auto generation =
                std::max(image.host_surface->cpu_generation(),
                    image.host_surface->gpu_generation());
            const auto writer = image.host_surface->writer();
            if ((!writer || writer->succeeded()) &&
                image.argb.size() ==
                    static_cast<std::size_t>(image.width) * image.height &&
                image.argb_generation == generation)
                continue;
            Source input { name, level, image.revision, image.native_lifetime,
                { }, { } };
            if (image.surface_id) {
                input.transport =
                    surfaces.acquire_transport_lease(*image.surface_id);
                if (!input.transport ||
                    surfaces.host_surface(*image.surface_id) !=
                        image.host_surface)
                    throw std::invalid_argument { "stale GLES shared texture" };
            } else if (!input.native_lifetime) {
                throw std::invalid_argument { "unowned GLES native texture" };
            }
            input.readback.surface = image.host_surface;
            sources_.push_back(std::move(input));
        }
}

bool GlesPreparedTextures::capture(
    const std::shared_ptr<GlesRenderer>& renderer)
{
    for (auto& input : sources_) {
        input.readback.renderer = renderer;
        if (!input.readback.capture(PerfCpuMapReason::SoftwareFallback))
            return false;
        const auto& image =
            textures_.texture(input.name)->levels.at(input.level);
        const auto& frame = input.readback.snapshot.frame;
        if (frame.width != image.width || frame.height != image.height ||
            frame.pixels.size() !=
                static_cast<std::size_t>(image.width) * image.height)
            return false;
    }
    return true;
}

void GlesPreparedTextures::complete(GlesResourceStore& revisions)
{
    if (completed_)
        return;
    for (auto& input : sources_) {
        auto& image = textures_.texture(input.name)->levels.at(input.level);
        if (image.revision != input.revision)
            throw std::logic_error { "GLES frozen texture identity changed" };
        // Allocate from the live monotonic namespace, never the packet's
        // fresh store. EGL uses this revision to invalidate uploaded pixels.
        const auto revision = revisions.allocate_texture_revision();
        auto& captured = input.readback;
        const auto generation = std::max(
            captured.snapshot.cpu_generation, captured.snapshot.gpu_generation);
        if (captured.surface->commit_cpu_readback(captured.snapshot)) {
            captured.renderer->acknowledge_readback(
                captured.surface->key(), captured.stamp);
            // Preserve the existing logical CPU cache only if it still names
            // this exact image/revision. The second capture-frame budget also
            // bounds this copy; the worker's temporary has already gone away.
            auto* live = revisions.texture(input.name);
            if (live) {
                const auto level = live->levels.find(input.level);
                if (level != live->levels.end() &&
                    level->second.revision == input.revision &&
                    level->second.host_surface == captured.surface) {
                    level->second.argb = captured.snapshot.frame.pixels;
                    level->second.host_generation = generation;
                    level->second.argb_generation = generation;
                    level->second.revision = revision;
                }
            }
        }
        image.argb = std::move(captured.snapshot.frame.pixels);
        image.host_generation = generation;
        image.argb_generation = image.host_generation;
        image.revision = revision;
    }
    completed_ = true;
    // CPU pixels now own all draw inputs. Release native/transport retention
    // on the core; their final releases only request worker retirement.
    sources_.clear();
}
} // namespace ilemu
