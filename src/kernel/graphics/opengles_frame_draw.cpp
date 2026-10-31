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
};

void OpenGlesHle::draw_frame_async(UserlandHleCall& call,
    const RenderTargetBinding& binding, const DisplayFrame& frame,
    std::span<const GlesRasterVertex> vertices, std::uint32_t mode,
    const GlesRasterState& raster, bool indexed)
try {
    std::size_t bytes = 0;
    auto host = GlesFrameDraw::prepare(
        frame, binding.key, vertices, mode, raster, bytes);
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
    if (pending->owner != renderer_owner_ || pending->binding.host_surface ||
        !binding_live(pending->binding))
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
                    !commit_render_target(resumed, pending->binding,
                        std::move(pending->result->frame)))
                    set_gl_error(resumed, gles_abi::invalid_operation);
            } catch (const std::bad_alloc&) {
                set_gl_error(resumed, gles_abi::out_of_memory);
            } catch (...) {
                set_gl_error(resumed, gles_abi::invalid_operation);
            }
            pending->lease->consume();
        },
        result->sequence);
} catch (...) {
    // If saving the observer fails after acceptance, keep its ticket in the
    // canceled lease until the normal core poll sees real host completion.
    pending->lease->cancelled = true;
    set_gl_error(call, gles_abi::invalid_operation);
}

} // namespace ilemu
