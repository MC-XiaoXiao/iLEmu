// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "graphics/surface_store.hpp"
#include "kernel/opengles_hle.hpp"
#include "opengles_dispatch_hle.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace ilemu {
namespace {
    constexpr std::size_t result_limit = 64U * 1024U * 1024U;
    std::size_t frame_bytes(std::uint32_t width, std::uint32_t height)
    {
        const auto pixels = static_cast<std::uint64_t>(width) * height;
        if (pixels > result_limit / sizeof(std::uint32_t))
            throw std::length_error { "GLES readback exceeds result budget" };
        return static_cast<std::size_t>(pixels) * sizeof(std::uint32_t);
    }

    // These handlers flush before effects. Draw target/alias changes and
    // MakeCurrent park at their validated preparation boundaries instead.
    bool flush_boundary(std::string_view symbol)
    {
        return symbol == "_glFinish" || symbol == "_glFlush" ||
               symbol == "_glReadPixels" || symbol == "_glClear" ||
               symbol == "_glRenderbufferStorage" ||
               symbol == "_glRenderbufferStorageOES" ||
               symbol == "_eglSwapBuffers" || symbol == "_eglDestroySurface" ||
               symbol == "_eglDestroyContext" ||
               symbol == "_glDeleteTextures" || symbol == "_glDeleteBuffers" ||
               symbol == "_glDeleteFramebuffersOES" ||
               symbol == "_glDeleteRenderbuffersOES" ||
               symbol == "_glDeleteFramebuffers" ||
               symbol == "_glDeleteRenderbuffers" ||
               symbol == "-[EAGLContext presentRenderbuffer:]" ||
               symbol == "-[EAGLContext renderbufferStorage:fromDrawable:]";
    }
}

bool OpenGlesHle::binding_live(const RenderTargetBinding& binding) const
{
    if (binding.backing_identifier) {
        const auto backing = surface_store_->find(*binding.backing_identifier);
        if (!backing ||
            binding.key != GlesRenderTargetKey {
                               0, backing->provenance.publication_sequence })
            return false;
    }
    if (binding.host_surface && binding.host_surface->key() != binding.key)
        return false;
    // Compare addresses and lifetime tokens, never a retired store entry.
    if (binding.pixmap_surface || binding.display_surface) {
        bool found =
            binding.display_surface == &compatibility_display_surface_ &&
            binding.surface_identity == compatibility_display_surface_.identity;
        for (const auto& [name, surface] : surfaces_) {
            static_cast<void>(name);
            found = found ||
                    ((&surface == binding.pixmap_surface ||
                         &surface == binding.display_surface) &&
                        binding.surface_identity == surface.identity);
        }
        if (!found)
            return false;
    }
    return true;
}

void OpenGlesHle::service_graphics_completions(bool process_live)
{
    // Poll every producer at the serial core boundary, including processes
    // with no ready guest observer. Ready draws must not pin the global result
    // budget merely because their producer has stopped making GL calls.
    if (program_renderer_) {
        if (process_live)
            collect_program_draw_errors();
        else
            static_cast<void>(program_renderer_->collect_errors());
    }
    if (sync_lease_ && sync_lease_->cancelled && sync_lease_->ready()) {
        // Retire canceled host payloads on the core even if the owning guest
        // has exited and no subsequent GL call will consume their credit.
        try {
            sync_lease_->consume();
        } catch (...) {
            // The retired observer has no ABI/error destination. Metadata
            // committers leave shared objects failed if allocation fails.
        }
        sync_lease_.reset();
    }
}

void OpenGlesHle::cancel_pending_work()
{
    if (sync_lease_) {
        sync_lease_->cancelled = true;
        sync_lease_->complete_core = { };
    }
}

void OpenGlesHle::enter_graphics_call(
    UserlandHleCall& call, UserlandHleRegistry::Handler handler)
{
    // Public wrappers only resolve TLS before their prefixed driver entry.
    // The actual driver entry captures output before any graphics wait.
    if (call.symbol().starts_with("_gl") && !call.prefix_argument(0U) &&
        dispatch_hle_->uses_context_first_dispatch()) {
        handler(call);
        return;
    }
    if (call.symbol() == "_glReadPixels") {
        try {
            const auto output = capture_read_pixels_output(call);
            handler = [this, output](UserlandHleCall& ready) {
                read_pixels(ready, output);
            };
        } catch (const std::bad_alloc&) {
            set_gl_error(call, gles_abi::out_of_memory);
            return;
        }
    }
    await_graphics_call(call, std::move(handler));
}

bool OpenGlesHle::needs_target_allocation(UserlandHleCall& call)
{
    const auto symbol = call.symbol();
    const auto present = symbol == "-[EAGLContext presentRenderbuffer:]";
    if (!present && symbol != "_glClear" && symbol != "_glDrawArrays" &&
        symbol != "_glDrawElements" && symbol != "_glReadPixels" &&
        symbol != "_glFinish" && symbol != "_glFlush")
        return false;
    const auto* context = present ? eagl_context(call) : current_context(call);
    if (!context)
        return false;
    auto framebuffer = context->framebuffers.find(context->bound_framebuffer);
    if (present) {
        if (call.argument(2) != gles_abi::renderbuffer ||
            !context->bound_renderbuffer)
            return false;
        const auto attached = std::find_if(context->framebuffers.begin(),
            context->framebuffers.end(), [context](const auto& entry) {
                return entry.second.color_renderbuffer ==
                       context->bound_renderbuffer;
            });
        if (attached != context->framebuffers.end())
            framebuffer = attached;
    }
    if (framebuffer == context->framebuffers.end())
        return false;
    const auto* texture = resources_.texture(framebuffer->second.color_texture);
    if (!texture)
        return false;
    const auto found = texture->levels.find(0U);
    if (found == texture->levels.end())
        return false;
    const auto& image = found->second;
    if (image.surface_id || !image.width || !image.height ||
        image.argb.size() != static_cast<std::uint64_t>(image.width) * image.height)
        return false;
    const auto writable = symbol == "_glClear" || symbol == "_glDrawArrays" ||
                          symbol == "_glDrawElements";
    return resources_.needs_texture_render_target(
        framebuffer->second.color_texture, 0U, writable);
}

void OpenGlesHle::await_graphics_call(
    UserlandHleCall& call, UserlandHleRegistry::Handler handler)
{
    if (!renderer_lifetime_->activate()) {
        call.suspend_until(
            [this] {
                return renderer_lifetime_->active() ||
                       GlesRendererOwner::capacity();
            },
            [this, handler = std::move(handler)](UserlandHleCall& resumed) {
                await_graphics_call(resumed, handler);
            });
        return;
    }
    service_graphics_completions();
    if (sync_lease_ && !sync_lease_->consumed) {
        // Only calls touching this process's graphics stores depend on this
        // lease. Other guest threads, devices and processes keep executing.
        const auto lease = sync_lease_;
        call.suspend_until([lease] { return lease->ready(); },
            [this, handler = std::move(handler)](UserlandHleCall& resumed) {
                await_graphics_call(resumed, handler);
            },
            lease->result ? lease->result->sequence : 0);
        return;
    }
    sync_lease_.reset();
    if (!GlesRendererOwner::capacity() && GlesRendererOwner::retirement_pending() &&
        needs_target_allocation(call)) {
        // No native target or handler effect has been accepted. Hold the
        // existing store gate while already retired GPU resources are reclaimed.
        // Deletion/binding/query calls do not need this creation credit.
        const auto lease = program_wait(call);
        call.suspend_until(
            [] {
                return GlesRendererOwner::capacity() ||
                       !GlesRendererOwner::retirement_pending();
            },
            [this, lease, handler = std::move(handler)](UserlandHleCall& resumed) {
                lease->consume();
                await_graphics_call(resumed, handler);
            });
        return;
    }
    if (call.symbol() == "_glGetError" && program_renderer_ &&
        !program_renderer_->issuer_ready(call.cpu().processor_id())) {
        const auto issuer = call.cpu().processor_id();
        const auto boundary = program_renderer_->issuer_boundary(issuer);
        call.suspend_until(
            [this, issuer, boundary] {
                return !program_renderer_ ||
                       program_renderer_->issuer_ready(issuer, boundary);
            },
            [handler = std::move(handler)](
                UserlandHleCall& resumed) { handler(resumed); },
            boundary);
        return;
    }
    if (pending_program_target_ && flush_boundary(call.symbol())) {
        await_program_flush(call, std::move(handler));
        return;
    }
    if (!prepare_texture_target(call, handler))
        handler(call);
}

void OpenGlesHle::await_program_flush(
    UserlandHleCall& call, UserlandHleRegistry::Handler handler)
{
    const auto& pending = *pending_program_target_;
    const auto key = pending.binding.key;
    std::size_t bytes;
    std::shared_ptr<GlesWorkResult> result;
    try {
        bytes = frame_bytes(pending.frame.width, pending.frame.height);
        if (program_renderer_)
            result = program_renderer_->enqueue_readback(key, bytes, renderer_);
    } catch (...) {
        fail_program_flush(call, handler, gles_abi::out_of_memory);
        return;
    }
    if (!program_renderer_) {
        fail_program_flush(call, handler, gles_abi::invalid_operation);
        return;
    }
    if (!result) {
        call.suspend_until([bytes] { return gles_host_work_capacity(bytes); },
            [this, handler = std::move(handler)](UserlandHleCall& resumed) {
                await_graphics_call(resumed, handler);
            });
        return;
    }
    auto lease = std::make_shared<SyncLease>();
    lease->owner = call.cpu().processor_id();
    lease->result = result;
    sync_lease_ = lease;
    call.suspend_until([result] { return result->ready(); },
        [this, result, lease, key, handler = std::move(handler)](
            UserlandHleCall& resumed) {
            if (!result->succeeded() || !pending_program_target_ ||
                pending_program_target_->binding.key != key ||
                !binding_live(pending_program_target_->binding)) {
                lease->consume();
                fail_program_flush(
                    resumed, handler, gles_abi::invalid_operation);
                return;
            }
            const auto committed = commit_render_target(resumed,
                pending_program_target_->binding, std::move(result->frame));
            collect_program_draw_errors();
            if (committed)
                pending_program_target_.reset();
            lease->consume();
            if (committed)
                // Other tasks may have consumed target capacity during the
                // readback. The completed target is already removed above.
                await_graphics_call(resumed, handler);
            else
                fail_program_flush(
                    resumed, handler, gles_abi::invalid_operation);
        },
        result->sequence);
}

void OpenGlesHle::fail_program_flush(UserlandHleCall& call,
    const UserlandHleRegistry::Handler& handler, std::uint32_t error)
{
    set_gl_error(call, error);
    if (call.symbol() == "_glFinish" || call.symbol() == "_glFlush" ||
        call.symbol() == "_glReadPixels" || call.symbol() == "_glDrawArrays" ||
        call.symbol() == "_glDrawElements")
        return;
    // Reuse the handler's EGL/GL failure ABI without resubmitting a readback.
    struct FailureScope {
        bool& value;
        ~FailureScope() { value = false; }
    } failure { program_flush_failed_ };
    program_flush_failed_ = true;
    handler(call);
}

void OpenGlesHle::finish_async(UserlandHleCall& call, bool finish)
{
    auto* context = current_context(call);
    const auto binding =
        context ? resolve_render_target(call, *context) : std::nullopt;
    if (!binding) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    const auto key = binding->key;
    auto result = enqueue_gles_host_work(
        0, [renderer = renderer_, key, finish](DisplayFrame&) {
            return finish ? renderer->finish(key) : renderer->flush(key);
        });
    if (!result) {
        call.suspend_until([] { return gles_host_work_capacity(0); },
            [this, finish](
                UserlandHleCall& resumed) { finish_async(resumed, finish); });
        return;
    }
    auto lease = std::make_shared<SyncLease>();
    lease->owner = call.cpu().processor_id();
    lease->result = result;
    sync_lease_ = lease;
    call.suspend_until([result] { return result->ready(); },
        [this, result, lease](UserlandHleCall& resumed) {
            if (!result->succeeded())
                set_gl_error(resumed, gles_abi::invalid_operation);
            lease->consume();
        },
        result->sequence);
}


} // namespace ilemu
