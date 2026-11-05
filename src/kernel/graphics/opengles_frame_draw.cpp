// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/opengles_hle.hpp"

#include "foundation/cpu.hpp"
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
    std::size_t issuer { };
    bool cancelled { };
    bool indexed { };
};

void OpenGlesHle::service_frame_draw(bool process_live)
{
    if (!pending_frame_draw_ || !pending_frame_draw_->result->ready())
        return;
    const auto pending = std::move(pending_frame_draw_);
    const auto succeeded = pending->host->complete();
    if (!process_live || pending->owner != renderer_owner_)
        return;
    if (succeeded) {
        if (binding_live(pending->binding) &&
            native_target_live(pending->binding) &&
            pending->binding.framebuffer_texture != 0U)
            resources_.update_texture_render_target_generation(
                pending->binding.framebuffer_texture, 0U);
    } else if (!pending->cancelled) {
        auto& error = threads_.at(pending->issuer).gl_error;
        if (error == gles_abi::no_error)
            error = gles_abi::invalid_operation;
    }
}

void OpenGlesHle::cancel_frame_draw(std::optional<std::size_t> processor)
{
    if (pending_frame_draw_ &&
        (!processor || pending_frame_draw_->issuer == *processor))
        pending_frame_draw_->cancelled = true;
}

std::shared_ptr<const GlesWorkCompletion> OpenGlesHle::frame_draw_dependency(
    std::optional<std::size_t> processor) const
{
    if (pending_frame_draw_ &&
        (!processor || pending_frame_draw_->issuer == *processor))
        return pending_frame_draw_->result->completion();
    return { };
}

void OpenGlesHle::draw_frame_async(UserlandHleCall& call,
    const RenderTargetBinding& binding, const DisplayFrame& frame,
    std::span<const GlesRasterVertex> vertices, std::uint32_t mode,
    const GlesRasterState& raster, bool indexed)
try {
    std::size_t bytes = 0;
    auto native_lifetime =
        binding.host_surface ? retain_native_target(binding) : nullptr;
    auto host = GlesFrameDraw::prepare(
        frame, binding.key, vertices, mode, raster, *surface_store_, bytes,
        binding.host_surface != nullptr, std::move(native_lifetime));
    if (!host) {
        // No command or snapshot was accepted. Only scalar ABI is retained;
        // preparation may restart after capacity returns and store dependencies
        // have been consumed, just as for program-draw preparation.
        call.suspend_for_host(
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
    pending->issuer = call.cpu().processor_id();
    pending->host = std::move(host);
    pending->lease = program_wait(call);
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
    if (pending->owner != renderer_owner_ || !binding_live(pending->binding) ||
        (pending->binding.host_surface &&
            !native_target_live(pending->binding)))
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
                call.suspend_for_host([] { return gles_host_work_capacity(0); },
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
            call.suspend_for_host(
                [host = pending->host] { return host->capacity(); },
                [this, pending](UserlandHleCall& resumed) {
                    await_frame_draw(resumed, pending);
                });
            return;
        }
        performance_counters().record_draw();
    }
    if (pending->binding.host_surface) {
        // The worker owns immutable inputs and the exact native target. Let
        // guest state preparation overlap it; target access and observation
        // consume this ticket before admitting another draw.
        pending_frame_draw_ = pending;
        pending->lease->consume();
        return;
    }
    pending->lease->result = pending->result;
    const auto result = pending->result;
    call.suspend_for_host([result] { return result->ready(); },
        [this, pending](UserlandHleCall& resumed) {
            try {
                if (pending->owner != renderer_owner_ ||
                    !binding_live(pending->binding) ||
                    !pending->result->succeeded() ||
                    (pending->binding.host_surface
                            ? !native_target_live(pending->binding)
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
