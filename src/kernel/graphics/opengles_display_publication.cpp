// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/opengles_hle.hpp"

#include "foundation/application_display.hpp"
#include "graphics/surface_store.hpp"
#include "kernel/kernel_shared_state.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace ilemu {
namespace {
    struct DisplayPublication {
        std::shared_ptr<void> credit, source_owner, output_owner;
        std::shared_ptr<GlesRenderer> renderer;
        std::shared_ptr<HostSurface> source, target;
        DisplayGeometry crop, output;
        DisplayViewport viewport;

        bool execute(DisplayFrame& result)
        {
            if (target) {
                const auto encoder = renderer->create_command_encoder();
                return encoder &&
                       encoder->fill(target,
                           { 0, 0, output.width, output.height },
                           0xff000000U) &&
                       encoder->copy(source, target,
                           { 0, 0, crop.width, crop.height },
                           { viewport.x, viewport.y, viewport.width,
                               viewport.height },
                           HostCompositeMode::Copy, 0xffU, HostFilter::Nearest,
                           HostRotation::Identity) &&
                       encoder->submit(PerfSubmitReason::Presentation);
            }
            DisplayFrame input;
            const auto descriptor = source->descriptor();
            if (!renderer->read_surface(
                    *source, input, PerfCpuMapReason::DeferredDisplayRead) ||
                input.width != descriptor.width ||
                input.height != descriptor.height ||
                input.pixels.size() !=
                    static_cast<std::size_t>(input.width) * input.height)
                return false;
            result = DisplayFrame { output.width, output.height, 0,
                std::vector<std::uint32_t>(output.pixel_count(), 0xff000000U) };
            for (std::uint32_t y = 0; y < viewport.height; ++y) {
                const auto source_y =
                    static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) *
                                               crop.height / viewport.height);
                for (std::uint32_t x = 0; x < viewport.width; ++x) {
                    const auto source_x = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(x) * crop.width /
                        viewport.width);
                    result.pixels[static_cast<std::size_t>(viewport.y + y) *
                                      output.width +
                                  viewport.x + x] =
                        input.pixels[static_cast<std::size_t>(source_y) *
                                         input.width +
                                     source_x];
                }
            }
            return true;
        }
    };
} // namespace

void OpenGlesHle::present_display_surface_async(UserlandHleCall& call,
    ResourceIdentity context, ResourceIdentity renderbuffer,
    std::uint32_t renderbuffer_name, const RenderTargetBinding& binding,
    std::optional<DisplayViewport> drawable_viewport)
try {
    const auto& surface = binding.host_surface;
    const auto live = [this, context, renderbuffer, renderbuffer_name, binding,
                          owner = renderer_owner_, display = display_] {
        if (!display || display_ != display || owner != renderer_owner_ ||
            !binding_live(binding))
            return false;
        const auto current =
            std::ranges::find_if(contexts_, [context](const auto& entry) {
                return entry.second.identity == context;
            });
        if (current == contexts_.end())
            return false;
        const auto buffer =
            current->second.renderbuffers.find(renderbuffer_name);
        return buffer != current->second.renderbuffers.end() &&
               buffer->second.identity == renderbuffer &&
               buffer->second.color_texture == binding.framebuffer_texture;
    };
    if (!surface || !live() || !display_write_allowed(call))
        throw std::invalid_argument { "stale display binding" };
    const auto descriptor = surface->descriptor();
    const auto output_geometry = display_->geometry();
    std::optional<ApplicationDisplay> profile;
    if (shared_state_) {
        std::lock_guard lock { shared_state_->mach_mutex };
        const auto process = shared_state_->processes.find(call.process_id());
        if (process != shared_state_->processes.end())
            profile = process->second.application_display;
    }
    const auto logical_geometry =
        profile ? application_display_geometry(*profile, output_geometry)
                : output_geometry;
    const auto is_geometry = [&](DisplayGeometry geometry) {
        return descriptor.width == geometry.width &&
               descriptor.height == geometry.height;
    };
    const auto compatibility =
        profile && profile->kind != ApplicationDisplayMode::Native;
    std::optional<DisplayViewport> viewport;
    auto source_width = descriptor.width;
    auto source_height = descriptor.height;
    if (compatibility) {
        if (!is_geometry(logical_geometry) && !is_geometry(output_geometry))
            throw std::invalid_argument { "invalid display viewport" };
        viewport = application_display_viewport(*profile, output_geometry);
        source_width = std::min(logical_geometry.width, descriptor.width);
        source_height = std::min(logical_geometry.height, descriptor.height);
    } else if (drawable_viewport) {
        viewport = drawable_viewport;
    } else if (!is_geometry(output_geometry)) {
        throw std::invalid_argument { "invalid display viewport" };
    }
    if (viewport &&
        (viewport->x < 0 || viewport->y < 0 || viewport->width == 0U ||
            viewport->height == 0U ||
            static_cast<std::uint64_t>(viewport->x) + viewport->width >
                output_geometry.width ||
            static_cast<std::uint64_t>(viewport->y) + viewport->height >
                output_geometry.height ||
            source_width == 0U || source_height == 0U)) {
        throw std::invalid_argument { "invalid display viewport" };
    }
    const auto needs_composition =
        viewport && (viewport->x != 0 || viewport->y != 0 ||
                        viewport->width != output_geometry.width ||
                        viewport->height != output_geometry.height ||
                        !is_geometry(output_geometry));
    const auto native =
        !needs_composition || (renderer_->accelerated() && command_encoder_);
    constexpr std::size_t limit = 64U * 1024U * 1024U;
    const auto input_bytes =
        needs_composition && !native
            ? static_cast<std::uint64_t>(descriptor.width) * descriptor.height *
                  sizeof(std::uint32_t)
            : 0U;
    const auto result_bytes =
        native ? 0U
               : static_cast<std::uint64_t>(output_geometry.width) *
                     output_geometry.height * sizeof(std::uint32_t);
    if (input_bytes > limit - sizeof(DisplayPublication) ||
        result_bytes > limit)
        throw std::length_error { "display publication exceeds frame budget" };
    const auto bytes =
        sizeof(DisplayPublication) + static_cast<std::size_t>(input_bytes);
    auto credit =
        needs_composition ? reserve_gles_host_preparation(bytes) : nullptr;
    if (needs_composition && !credit) {
        const auto lease = program_wait(call);
        call.suspend_until(
            [bytes] { return gles_host_preparation_capacity(bytes); },
            [this, lease, context, renderbuffer, renderbuffer_name, binding,
                drawable_viewport](UserlandHleCall& resumed) {
                lease->consume();
                present_display_surface_async(resumed, context, renderbuffer,
                    renderbuffer_name, binding, drawable_viewport);
            });
        return;
    }
    auto work = std::make_shared<DisplayPublication>();
    work->credit = std::move(credit);
    work->source = surface;
    work->renderer = renderer_;
    if (binding.backing_identifier) {
        work->source_owner = surface_store_->acquire_transport_lease(
            *binding.backing_identifier);
    } else {
        const auto* texture = resources_.texture(binding.framebuffer_texture);
        if (!texture || !texture->levels.contains(0U) ||
            texture->levels.at(0U).host_surface != surface)
            throw std::invalid_argument { "display source replaced" };
        work->source_owner = texture->levels.at(0U).native_lifetime;
    }
    if (!work->source_owner)
        throw std::invalid_argument { "unowned display source" };
    work->crop = { source_width, source_height };
    work->output = output_geometry;
    if (viewport)
        work->viewport = *viewport;
    auto published_surface = native ? surface : nullptr;
    if (needs_composition && native) {
        work->target = acquire_compatibility_surface(
            { output_geometry.width, output_geometry.height,
                output_geometry.width *
                    static_cast<std::uint32_t>(sizeof(std::uint32_t)),
                surface_pixel_format_bgra, PerfSurfaceKind::Scanout });
        if (!work->target)
            throw std::runtime_error { "missing display target" };
        published_surface = work->target;
    }
    work->output_owner = needs_composition
                             ? std::shared_ptr<void> { renderer_lifetime_ }
                             : work->source_owner;
    auto completed = [this, work, live, published_surface, needs_composition](
                         UserlandHleCall& resumed,
                         const std::shared_ptr<GlesWorkResult>& result) {
        try {
            if (!live() || !display_write_allowed(resumed) ||
                (needs_composition && (!result || !result->succeeded()))) {
                resumed.set_return(0U);
                return;
            }
            if (!published_surface) {
                display_->replace_pixels(
                    std::move(result->frame.pixels), resumed.process_id());
            } else {
                auto presentation = make_host_surface_presentation_lease(
                    published_surface, work->output_owner);
                display_->replace_surface(
                    published_surface,
                    [renderer = work->renderer, published_surface] {
                        if (!renderer->map_cpu(*published_surface, true,
                                PerfCpuMapReason::DeferredDisplayRead))
                            return std::vector<std::uint32_t> { };
                        auto mapping = published_surface->map_cpu(
                            false, PerfCpuMapReason::DeferredDisplayRead);
                        return mapping.frame().pixels;
                    },
                    resumed.process_id(),
                    [published_surface] {
                        return std::max(published_surface->cpu_generation(),
                            published_surface->gpu_generation());
                    },
                    std::move(presentation));
            }
            display_->present(resumed.process_id());
            resumed.set_return(1U);
        } catch (...) {
            resumed.set_return(0U);
        }
    };
    if (!needs_composition) {
        completed(call, { });
        return;
    }
    submit_host_work_async(
        call, static_cast<std::size_t>(result_bytes),
        [work](DisplayFrame& output) { return work->execute(output); },
        std::move(completed), { }, work->source, work->target);
} catch (...) {
    call.set_return(0U);
}

} // namespace ilemu
