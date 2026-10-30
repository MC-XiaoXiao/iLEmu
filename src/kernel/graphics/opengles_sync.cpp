// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "graphics/surface_store.hpp"
#include "kernel/opengles_hle.hpp"
#include "opengles_dispatch_hle.hpp"

#include <limits>
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
    // Compare addresses only: never dereference a possibly retired store entry.
    if (binding.pixmap_surface || binding.display_surface) {
        bool found = binding.display_surface == &compatibility_display_surface_;
        for (const auto& [name, surface] : surfaces_) {
            static_cast<void>(name);
            found = found || &surface == binding.pixmap_surface ||
                    &surface == binding.display_surface;
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
    service_graphics_completions();
    if (sync_lease_ && !sync_lease_->consumed) {
        // Only calls touching this process's graphics stores depend on this
        // lease. Other guest threads, devices and processes keep executing.
        const auto lease = sync_lease_;
        call.suspend_until([lease] { return lease->ready(); },
            [this, handler = std::move(handler)](UserlandHleCall& resumed) {
                enter_graphics_call(resumed, handler);
            },
            lease->result ? lease->result->sequence : 0);
        return;
    }
    sync_lease_.reset();
    if (call.symbol().starts_with("_gl") && !call.prefix_argument(0U) &&
        dispatch_hle_->uses_context_first_dispatch()) {
        handler(call);
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
                enter_graphics_call(resumed, handler);
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
                handler(resumed);
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

void OpenGlesHle::read_pixels(UserlandHleCall& call)
{
    auto* context = current_context(call);
    if (!context) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    const auto x = static_cast<std::int32_t>(call.argument(0));
    const auto y = static_cast<std::int32_t>(call.argument(1));
    const auto width = static_cast<std::int32_t>(call.argument(2));
    const auto height = static_cast<std::int32_t>(call.argument(3));
    if ((call.argument(4) != gles_abi::rgba &&
            call.argument(4) != gles_abi::bgra_apple) ||
        call.argument(5) != gles_abi::unsigned_byte) {
        set_gl_error(call, gles_abi::invalid_enum);
        return;
    }
    if (x < 0 || y < 0 || width < 0 || height < 0 ||
        (width && height && call.argument(6) == 0)) {
        set_gl_error(call, gles_abi::invalid_value);
        return;
    }
    if (!width || !height)
        return;
    const auto binding = resolve_render_target(call, *context);
    if (!binding) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    const auto display_source = binding->kind == RenderTargetKind::Display &&
                                !binding->display_surface &&
                                !binding->pixmap_surface;
    auto frame = display_source && display_
                     ? std::optional { display_->capture_frame() }
                     : render_target(call, *binding);
    if (!frame) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    std::size_t bytes;
    try {
        bytes = frame_bytes(frame->width, frame->height);
    } catch (...) {
        set_gl_error(call, gles_abi::out_of_memory);
        return;
    }
    // Guest/reader callbacks never cross; retain the host content lease
    // separately from the plain CPU dimensions/pixels input.
    DisplayFrame snapshot { frame->width, frame->height, 0, { } };
    const auto surface =
        binding->host_surface ? binding->host_surface : frame->host_surface;
    if (!surface)
        snapshot.pixels = std::move(frame->pixels);
    auto result = enqueue_gles_host_work(bytes,
        [renderer = renderer_, surface,
            source_lease = std::move(frame->presentation_lease),
            synchronize_target = !binding->host_surface, key = binding->key,
            frame = std::move(snapshot)](DisplayFrame& output) mutable {
            static_cast<void>(source_lease);
            if (surface) {
                if (!renderer->map_cpu(*surface, true))
                    return false;
                {
                    auto mapping = surface->map_cpu(false);
                    output.width = mapping.frame().width;
                    output.height = mapping.frame().height;
                    output.pixels = mapping.frame().pixels;
                }
                // Preserve snapshot()+synchronize order without a core wait.
                return !synchronize_target ||
                       renderer->synchronize(output, key);
            }
            output = std::move(frame);
            return renderer->synchronize(output, key);
        });
    if (!result) {
        call.suspend_until([bytes] { return gles_host_work_capacity(bytes); },
            [this](UserlandHleCall& resumed) { read_pixels(resumed); });
        return;
    }
    auto lease = std::make_shared<SyncLease>();
    lease->owner = call.cpu().processor_id();
    lease->result = result;
    sync_lease_ = lease;
    const auto alignment = context->pack_alignment;
    call.suspend_until([result] { return result->ready(); },
        [this, result, lease, inverted = binding->inverted_vertical, alignment,
            backing = binding->backing_identifier,
            key = binding->key](UserlandHleCall& resumed) {
            const auto current =
                backing ? surface_store_->find(*backing) : std::nullopt;
            const bool live =
                !backing ||
                (current &&
                    current->provenance.publication_sequence == key.surface);
            if (!result->succeeded() || !live)
                set_gl_error(resumed, gles_abi::invalid_operation);
            else
                write_read_pixels(resumed, result->frame, inverted, alignment);
            lease->consume();
        },
        result->sequence);
}

} // namespace ilemu
