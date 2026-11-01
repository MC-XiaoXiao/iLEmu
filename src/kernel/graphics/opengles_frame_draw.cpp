// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/opengles_hle.hpp"

#include "graphics/gles_frame_draw.hpp"
#include "graphics/surface_store.hpp"

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
    bool indexed { };

    static bool target_live(const GlesResourceStore& resources,
        const SurfaceStore& surfaces, const RenderTargetBinding& binding)
    {
        if (binding.kind == RenderTargetKind::Pixmap)
            return binding.backing_identifier &&
                   surfaces.host_surface(*binding.backing_identifier) ==
                       binding.host_surface;
        const auto* texture = resources.texture(binding.framebuffer_texture);
        if (!texture)
            return false;
        const auto level = texture->levels.find(0U);
        return level != texture->levels.end() &&
               level->second.host_surface == binding.host_surface &&
               (binding.backing_identifier
                       ? level->second.surface_id == binding.backing_identifier
                       : !level->second.surface_id &&
                             level->second.native_lifetime);
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
        if (!FrameDraw::target_live(resources_, *surface_store_, binding))
            throw std::runtime_error { "stale native GLES frame target" };
        native_lifetime = binding.backing_identifier
                              ? surface_store_->acquire_transport_lease(
                                    *binding.backing_identifier)
                              : std::shared_ptr<void> {
                                    resources_.texture(binding.framebuffer_texture)
                                        ->levels.at(0U).native_lifetime };
        if (!native_lifetime)
            throw std::runtime_error { "missing GLES frame target owner" };
    }
    auto host = GlesFrameDraw::prepare(
        frame, binding.key, vertices, mode, raster, *surface_store_, bytes,
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
    pending->indexed = indexed;
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
                binding_live(binding) &&
                FrameDraw::target_live(resources_, *surface_store_, binding) &&
                binding.framebuffer_texture != 0U)
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
            !FrameDraw::target_live(resources_, *surface_store_, pending->binding)))
        throw std::runtime_error { "stale GLES frame target" };
    if (!pending->result) {
        pending->result = pending->host->enqueue(renderer_);
        if (!pending->result) {
            if (pending->binding.host_surface) {
                // A shared source can change while admission is unavailable.
                // Nothing was accepted: release live inputs, keep the store
                // gate, and prepare again after capacity returns.
                pending->lease->complete_core = { };
                pending->host.reset();
                call.suspend_until([] { return gles_host_work_capacity(0); },
                    [this, lease = pending->lease, indexed = pending->indexed](
                        UserlandHleCall& resumed) {
                        lease->consume();
                        await_graphics_call(resumed,
                            [this, indexed](UserlandHleCall& ready) {
                                draw(ready, indexed);
                            });
                    });
                return;
            }
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
                            ? !FrameDraw::target_live(
                                  resources_, *surface_store_, pending->binding)
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
