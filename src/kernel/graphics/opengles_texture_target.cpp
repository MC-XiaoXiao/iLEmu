// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/userland_hle.hpp"
#include "graphics/gles_prepared_textures.hpp"
#include "graphics/gles_primitive_assembler.hpp"
#include "kernel/opengles_hle.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace ilemu {
struct OpenGlesHle::TextureTargetInput {
    ResourceIdentity context;
    std::uint64_t owner, revision;
    std::uint32_t texture;
    std::shared_ptr<HostSurface> surface;
    std::shared_ptr<GlesPreparedTextures> pixels;
    std::shared_ptr<void> credit;
    std::size_t bytes;
    std::shared_ptr<GlesWorkResult> result;
};

bool OpenGlesHle::prepare_texture_target(
    UserlandHleCall& call, const UserlandHleRegistry::Handler& handler)
{
    const auto symbol = call.symbol();
    if (symbol != "_glClear" && symbol != "_glDrawArrays" &&
        symbol != "_glDrawElements")
        return false;
    if (symbol == "_glDrawArrays" || symbol == "_glDrawElements") {
        const auto count = static_cast<std::int32_t>(
            call.argument(symbol == "_glDrawArrays" ? 2U : 1U));
        if (count <= 0 ||
            static_cast<std::uint32_t>(count) <
                GlesPrimitiveAssembler::minimum_vertex_count(
                    call.argument(0)) ||
            (symbol == "_glDrawArrays" &&
                static_cast<std::int32_t>(call.argument(1)) < 0))
            return false;
    }
    if (symbol == "_glClear" &&
        ((call.argument(0) & gles_abi::color_buffer_bit) == 0U ||
            (call.argument(0) &
                ~(gles_abi::color_buffer_bit | gles_abi::depth_buffer_bit |
                    gles_abi::stencil_buffer_bit)) != 0U))
        return false;
    const auto* context = current_context(call);
    if (!context)
        return false;
    const auto framebuffer =
        context->framebuffers.find(context->bound_framebuffer);
    if (framebuffer == context->framebuffers.end())
        return false;
    const auto name = framebuffer->second.color_texture;
    const auto* texture = resources_.texture(name);
    if (!texture)
        return false;
    const auto level = texture->levels.find(0U);
    if (level == texture->levels.end())
        return false;
    const auto& image = level->second;
    if (image.surface_id || !image.host_surface || !image.native_lifetime)
        return false;
    const auto cpu_clear =
        symbol == "_glClear" &&
        (!renderer_->accelerated() || !command_encoder_ ||
            !std::all_of(context->color_mask.begin(), context->color_mask.end(),
                [](bool enabled) { return enabled; }));
    if (!cpu_clear && !resources_.needs_texture_render_target(name, 0U, true))
        return false;
    if (image.argb_generation == std::max(image.host_surface->cpu_generation(),
                                     image.host_surface->gpu_generation()) &&
        image.argb.size() ==
            static_cast<std::size_t>(image.width) * image.height)
        return false;
    try {
        constexpr std::size_t limit = 64U * 1024U * 1024U;
        const std::array names { name };
        const auto bytes =
            sizeof(TextureTargetInput) + sizeof(GlesPreparedTextures) +
            GlesPreparedTextures::payload_bytes(resources_, names,
                limit - sizeof(TextureTargetInput) -
                    sizeof(GlesPreparedTextures));
        auto credit = reserve_gles_host_preparation(bytes);
        if (!credit) {
            const auto lease = program_wait(call);
            call.suspend_until(
                [bytes] { return gles_host_preparation_capacity(bytes); },
                [this, lease, handler](UserlandHleCall& resumed) {
                    lease->consume();
                    await_graphics_call(resumed, handler);
                });
            return true;
        }
        auto pending = std::make_shared<TextureTargetInput>();
        pending->context = context->identity;
        pending->owner = renderer_owner_;
        pending->texture = name;
        pending->revision = image.revision;
        pending->surface = image.host_surface;
        pending->bytes = bytes;
        pending->credit = std::move(credit);
        pending->pixels = std::make_shared<GlesPreparedTextures>(
            resources_, names, *surface_store_);
        await_texture_target(call, std::move(pending), handler);
    } catch (const std::bad_alloc&) {
        if (sync_lease_)
            sync_lease_->cancelled = true;
        set_gl_error(call, gles_abi::out_of_memory);
    } catch (const std::length_error&) {
        if (sync_lease_)
            sync_lease_->cancelled = true;
        set_gl_error(call, gles_abi::out_of_memory);
    } catch (...) {
        if (sync_lease_)
            sync_lease_->cancelled = true;
        set_gl_error(call, gles_abi::invalid_operation);
    }
    return true;
}

void OpenGlesHle::await_texture_target(UserlandHleCall& call,
    std::shared_ptr<TextureTargetInput> pending,
    UserlandHleRegistry::Handler handler)
{
    const auto lease = program_wait(call);
    try {
        if (!pending->result)
            pending->result = enqueue_gles_host_work(
                0,
                [pixels = pending->pixels, credit = pending->credit,
                    renderer = renderer_](
                    DisplayFrame&) { return pixels->capture(renderer); },
                pending->bytes);
    } catch (...) {
        lease->consume();
        set_gl_error(call, gles_abi::out_of_memory);
        return;
    }
    if (!pending->result) {
        call.suspend_until(
            [bytes = pending->bytes] {
                return gles_host_work_capacity(0, bytes);
            },
            [this, pending, handler](UserlandHleCall& resumed) {
                await_texture_target(resumed, pending, handler);
            });
        return;
    }
    lease->result = pending->result;
    call.suspend_until([result = pending->result] { return result->ready(); },
        [this, pending, lease, handler](UserlandHleCall& resumed) {
            const auto* context = current_context(resumed);
            const auto* texture = resources_.texture(pending->texture);
            const GlesResourceStore::TextureLevel* image = nullptr;
            if (texture) {
                const auto level = texture->levels.find(0U);
                if (level != texture->levels.end())
                    image = &level->second;
            }
            bool attached = false;
            if (context) {
                const auto framebuffer =
                    context->framebuffers.find(context->bound_framebuffer);
                attached =
                    framebuffer != context->framebuffers.end() &&
                    framebuffer->second.color_texture == pending->texture;
            }
            if (!attached || !pending->result->succeeded() ||
                renderer_owner_ != pending->owner || !context ||
                context->identity != pending->context || !image ||
                image->revision != pending->revision ||
                image->host_surface != pending->surface) {
                lease->consume();
                set_gl_error(resumed, gles_abi::invalid_operation);
                return;
            }
            try {
                pending->pixels->complete(resources_);
                // Drop temporary reader ownership before deciding whether the
                // logical target is still shared with another fork or job.
                pending->pixels.reset();
                pending->credit.reset();
                lease->consume();
            } catch (const std::bad_alloc&) {
                lease->consume();
                set_gl_error(resumed, gles_abi::out_of_memory);
                return;
            } catch (...) {
                lease->consume();
                set_gl_error(resumed, gles_abi::invalid_operation);
                return;
            }
            await_graphics_call(resumed, handler);
        },
        pending->result->sequence());
}
} // namespace ilemu
