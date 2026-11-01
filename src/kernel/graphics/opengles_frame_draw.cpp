// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/opengles_hle.hpp"

#include "graphics/gles_frame_draw.hpp"

#include <new>
#include <stdexcept>
#include <utility>

namespace ilemu {

struct OpenGlesHle::FrameDraw {
    RenderTargetBinding binding;
    std::uint64_t owner { };
    std::shared_ptr<GlesFrameDraw> host;
    std::shared_ptr<GlesWorkResult> result;
    std::shared_ptr<SyncLease> lease;

    static bool texture_target_live(const GlesResourceStore& resources,
        const RenderTargetBinding& binding)
    {
        const auto* texture = resources.texture(binding.framebuffer_texture);
        if (!texture)
            return false;
        const auto level = texture->levels.find(0U);
        return level != texture->levels.end() && !level->second.surface_id &&
               level->second.host_surface == binding.host_surface &&
               level->second.native_lifetime;
    }
};

void OpenGlesHle::draw_frame_async(UserlandHleCall& call,
    const RenderTargetBinding& binding, const DisplayFrame& frame,
    std::span<const GlesRasterVertex> vertices, std::uint32_t mode,
    const GlesRasterState& raster, bool indexed)
try {
    std::size_t bytes = 0;
    std::shared_ptr<void> native_lifetime;
    if (binding.host_surface) {
        if (!FrameDraw::texture_target_live(resources_, binding))
            throw std::runtime_error { "stale native GLES frame target" };
        native_lifetime = resources_.texture(binding.framebuffer_texture)
                              ->levels.at(0U)
                              .native_lifetime;
    }
    auto host = GlesFrameDraw::prepare(
        frame, binding.key, vertices, mode, raster, bytes,
        std::move(native_lifetime));
    if (!host) {
        // No command or snapshot was accepted. Only scalar ABI is retained;
        // preparation may restart after capacity returns and store dependencies
        // have been consumed, just as for program-draw preparation.
        call.suspend_until(
            [bytes] { return gles_host_preparation_capacity(bytes); },
            [this, indexed](UserlandHleCall& resumed) {
                await_graphics_call(
                    resumed, [this, indexed](UserlandHleCall& ready) {
                        draw(ready, indexed);
                    });
            });
        return;
    }
    auto pending = std::make_shared<FrameDraw>();
    pending->binding = binding;
    pending->owner = renderer_owner_;
    pending->host = std::move(host);
    std::function<void()> complete_native;
    if (binding.host_surface) {
        // Allocate the core committer before accepting work or taking its
        // store gate. It survives a thread exit, and process exit/exec clears
        // it through the existing lease cancellation path.
        complete_native = [this, binding, owner = pending->owner,
                              host = pending->host] {
            if (host->complete() && owner == renderer_owner_ &&
                FrameDraw::texture_target_live(resources_, binding))
                resources_.update_texture_render_target_generation(
                    binding.framebuffer_texture, 0U);
        };
    }
    pending->lease = program_wait(call);
    pending->lease->complete_core = std::move(complete_native);
    await_frame_draw(call, std::move(pending));
} catch (const std::bad_alloc&) {
    set_gl_error(call, gles_abi::out_of_memory);
} catch (const std::length_error&) {
    set_gl_error(call, gles_abi::out_of_memory);
} catch (...) {
    set_gl_error(call, gles_abi::invalid_operation);
}

void OpenGlesHle::await_frame_draw(
    UserlandHleCall& call, std::shared_ptr<FrameDraw> pending)
try {
    if (pending->owner != renderer_owner_ ||
        !binding_live(pending->binding) ||
        (pending->binding.host_surface &&
            !FrameDraw::texture_target_live(resources_, pending->binding)))
        throw std::runtime_error { "stale GLES frame target" };
    if (!pending->result) {
        pending->result = pending->host->enqueue(renderer_);
        if (!pending->result) {
            call.suspend_until(
                [host = pending->host] { return host->capacity(); },
                [this, pending](UserlandHleCall& resumed) {
                    await_frame_draw(resumed, pending);
                });
            return;
        }
        pending->lease->result = pending->result;
        performance_counters().record_draw();
    }
    const auto result = pending->result;
    call.suspend_until([result] { return result->ready(); },
        [this, pending](UserlandHleCall& resumed) {
            try {
                if (pending->owner != renderer_owner_ ||
                    !binding_live(pending->binding) ||
                    !pending->result->succeeded() ||
                    (pending->binding.host_surface
                            ? !FrameDraw::texture_target_live(
                                  resources_, pending->binding)
                            : !commit_render_target(resumed, pending->binding,
                                  std::move(pending->result->frame))))
                    set_gl_error(resumed, gles_abi::invalid_operation);
            } catch (const std::bad_alloc&) {
                set_gl_error(resumed, gles_abi::out_of_memory);
            } catch (...) {
                set_gl_error(resumed, gles_abi::invalid_operation);
            }
            pending->lease->consume();
        },
        result->sequence());
} catch (...) {
    // If saving the observer fails after acceptance, keep its ticket in the
    // canceled lease until the normal core poll sees real host completion.
    pending->lease->cancelled = true;
    set_gl_error(call, gles_abi::invalid_operation);
}

} // namespace ilemu
