// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include "foundation/userland_hle.hpp"
#include "kernel/opengles_hle.hpp"

#include <limits>

namespace ilemu {
namespace {
    constexpr std::string_view opengles_image {
        "/OpenGLES.framework/OpenGLES"
    };
    constexpr std::uint32_t gl_renderbuffer_width = 0x8d42;
    constexpr std::uint32_t gl_renderbuffer_height = 0x8d43;
    constexpr std::uint32_t gl_renderbuffer_internal_format = 0x8d44;
    constexpr std::uint32_t gl_renderbuffer_color_format = 0x8e10;
    template <typename Objects>
    std::uint32_t available_name(const Objects& objects)
    {
        // Names released by deletion are available to both core and OES calls.
        std::uint32_t name = 1U;
        for (const auto& [used, object] : objects) {
            static_cast<void>(object);
            if (used > name)
                break;
            if (used == name && ++name == 0U)
                return 0U;
        }
        return name;
    }
} // namespace

const GlesResourceStore::TextureLevel* OpenGlesHle::current_framebuffer_level(
    const ContextState& context) const
{
    if (context.bound_framebuffer == 0U)
        return nullptr;
    const auto framebuffer =
        context.framebuffers.find(context.bound_framebuffer);
    if (framebuffer == context.framebuffers.end() ||
        framebuffer->second.color_texture == 0U) {
        return nullptr;
    }
    const auto* texture = resources_.texture(framebuffer->second.color_texture);
    if (texture == nullptr)
        return nullptr;
    const auto level = texture->levels.find(0U);
    if (level == texture->levels.end() || level->second.width == 0U ||
        level->second.height == 0U)
        return nullptr;
    const auto complete = [&](std::uint32_t name, bool depth) {
        if (name == 0U)
            return true;
        const auto buffer = context.renderbuffers.find(name);
        if (buffer == context.renderbuffers.end() ||
            buffer->second.width != level->second.width ||
            buffer->second.height != level->second.height)
            return false;
        const auto format = buffer->second.internal_format;
        return format == gles_abi::depth24_stencil8 ||
               (depth ? format == gles_abi::depth_component16 ||
                            format == gles_abi::depth_component24
                      : format == gles_abi::stencil_index8);
    };
    return complete(framebuffer->second.depth_renderbuffer, true) &&
                   complete(framebuffer->second.stencil_renderbuffer, false)
               ? &level->second
               : nullptr;
}

std::uint32_t OpenGlesHle::ensure_renderbuffer_storage(ContextState& context,
    std::uint32_t name, std::uint32_t width, std::uint32_t height,
    std::uint32_t internal_format)
{
    auto renderbuffer = context.renderbuffers.find(name);
    if (renderbuffer == context.renderbuffers.end())
        return gles_abi::invalid_operation;
    if (renderbuffer->second.color_texture == 0U) {
        renderbuffer->second.color_texture = resources_.generate_texture();
        resources_.ensure_texture(renderbuffer->second.color_texture);
    }
    const auto error = resources_.allocate_texture_2d(
        renderbuffer->second.color_texture, 0U, internal_format, width, height);
    if (error != gles_abi::no_error)
        return error;
    renderbuffer->second.width = width;
    renderbuffer->second.height = height;
    renderbuffer->second.internal_format = internal_format;
    renderbuffer->second.display_oriented = false;
    renderbuffer->second.native_window = 0U;
    renderbuffer->second.drawable_surface_id.reset();
    for (auto& [framebuffer_name, framebuffer] : context.framebuffers) {
        static_cast<void>(framebuffer_name);
        if (framebuffer.color_renderbuffer == name)
            framebuffer.color_texture = renderbuffer->second.color_texture;
    }
    return gles_abi::no_error;
}

void OpenGlesHle::register_framebuffers(UserlandHleRegistry& registry)
{
    const auto add = [&](std::string_view name,
                         UserlandHleRegistry::Handler handler) {
        registry.register_function(std::string { opengles_image },
            std::string { name }, std::move(handler));
    };
    const auto generate_framebuffers = [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        const auto count = static_cast<std::int32_t>(call.argument(0));
        const auto output = call.argument(1);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (count < 0 || (count != 0 && output == 0U)) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        for (std::int32_t index = 0; index < count; ++index) {
            const auto name = available_name(context->framebuffers);
            if (name == 0U) {
                set_gl_error(call, gles_abi::out_of_memory);
                return;
            }
            context->framebuffers.try_emplace(name);
            if (!call.memory().write32(
                    output + static_cast<std::uint32_t>(index) * 4U, name)) {
                context->framebuffers.erase(name);
                set_gl_error(call, gles_abi::invalid_value);
                return;
            }
        }
    };
    add("_glGenFramebuffers", generate_framebuffers);
    add("_glGenFramebuffersOES", generate_framebuffers);
    const auto is_framebuffer = [this](UserlandHleCall& call) {
        const auto* context = current_context(call);
        call.set_return(context != nullptr &&
                                context->framebuffers.contains(call.argument(0))
                            ? 1U
                            : 0U);
    };
    add("_glIsFramebuffer", is_framebuffer);
    add("_glIsFramebufferOES", is_framebuffer);
    const auto bind_framebuffer = [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (call.argument(0) != gles_abi::framebuffer) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        const auto name = call.argument(1);
        if (name != 0U)
            context->framebuffers.try_emplace(name);
        context->bound_framebuffer = name;
    };
    add("_glBindFramebuffer", bind_framebuffer);
    add("_glBindFramebufferOES", bind_framebuffer);
    const auto framebuffer_texture_2d = [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto target = call.argument(0);
        const auto attachment = call.argument(1);
        const auto texture_target = call.argument(2);
        const auto texture = call.argument(3);
        const auto level = call.argument(4);
        if (target != gles_abi::framebuffer ||
            attachment != gles_abi::color_attachment0 ||
            (texture_target != gles_abi::texture_2d &&
                texture_target != gles_abi::texture_rectangle_apple)) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        if (context->bound_framebuffer == 0U || level != 0U ||
            (texture != 0U && !resources_.has_texture(texture))) {
            set_gl_error(call, level != 0U ? gles_abi::invalid_value
                                           : gles_abi::invalid_operation);
            return;
        }
        auto& framebuffer = context->framebuffers[context->bound_framebuffer];
        framebuffer.color_texture_target = texture_target;
        framebuffer.color_texture = texture;
        framebuffer.color_renderbuffer = 0U;
    };
    add("_glFramebufferTexture2D", framebuffer_texture_2d);
    add("_glFramebufferTexture2DOES", framebuffer_texture_2d);
    const auto check_framebuffer_status = [this](UserlandHleCall& call) {
        const auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            call.set_return(0U);
            return;
        }
        if (call.argument(0) != gles_abi::framebuffer) {
            set_gl_error(call, gles_abi::invalid_enum);
            call.set_return(0U);
            return;
        }
        if (context->bound_framebuffer == 0U) {
            call.set_return(gles_abi::framebuffer_complete);
            return;
        }
        const auto complete = current_framebuffer_level(*context) != nullptr;
        call.set_return(complete ? gles_abi::framebuffer_complete
                                 : gles_abi::framebuffer_incomplete_attachment);
    };
    add("_glCheckFramebufferStatus", check_framebuffer_status);
    add("_glCheckFramebufferStatusOES", check_framebuffer_status);
    const auto delete_framebuffers = [this](UserlandHleCall& call) {
        if (!flush_program_draws(call))
            return;
        auto* context = current_context(call);
        const auto count = static_cast<std::int32_t>(call.argument(0));
        const auto input = call.argument(1);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (count < 0 || (count != 0 && input == 0U)) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        for (std::int32_t index = 0; index < count; ++index) {
            const auto name = call.memory().read32(
                input + static_cast<std::uint32_t>(index) * 4U);
            if (!name) {
                set_gl_error(call, gles_abi::invalid_value);
                return;
            }
            context->framebuffers.erase(*name);
            if (context->bound_framebuffer == *name)
                context->bound_framebuffer = 0U;
        }
    };
    add("_glDeleteFramebuffers", delete_framebuffers);
    add("_glDeleteFramebuffersOES", delete_framebuffers);
    const auto generate_renderbuffers = [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        const auto count = static_cast<std::int32_t>(call.argument(0));
        const auto output = call.argument(1);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (count < 0 || (count != 0 && output == 0U)) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        for (std::int32_t index = 0; index < count; ++index) {
            const auto name = available_name(context->renderbuffers);
            if (name == 0U) {
                set_gl_error(call, gles_abi::out_of_memory);
                return;
            }
            context->renderbuffers.try_emplace(name);
            if (!call.memory().write32(
                    output + static_cast<std::uint32_t>(index) * 4U, name)) {
                context->renderbuffers.erase(name);
                set_gl_error(call, gles_abi::invalid_value);
                return;
            }
        }
    };
    add("_glGenRenderbuffers", generate_renderbuffers);
    add("_glGenRenderbuffersOES", generate_renderbuffers);
    const auto is_renderbuffer = [this](UserlandHleCall& call) {
        const auto* context = current_context(call);
        call.set_return(context != nullptr && context->renderbuffers.contains(
                                                  call.argument(0))
                            ? 1U
                            : 0U);
    };
    add("_glIsRenderbuffer", is_renderbuffer);
    add("_glIsRenderbufferOES", is_renderbuffer);
    const auto bind_renderbuffer = [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (call.argument(0) != gles_abi::renderbuffer) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        const auto name = call.argument(1);
        if (name != 0U && !context->renderbuffers.contains(name)) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        context->bound_renderbuffer = name;
    };
    add("_glBindRenderbuffer", bind_renderbuffer);
    add("_glBindRenderbufferOES", bind_renderbuffer);
    const auto renderbuffer_storage = [this](UserlandHleCall& call) {
        if (!flush_program_draws(call))
            return;
        auto* context = current_context(call);
        const auto width = call.argument(2);
        const auto height = call.argument(3);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (call.argument(0) != gles_abi::renderbuffer ||
            context->bound_renderbuffer == 0U) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        const auto error = ensure_renderbuffer_storage(*context,
            context->bound_renderbuffer, width, height, call.argument(1));
        if (error != gles_abi::no_error)
            set_gl_error(call, error);
    };
    add("_glRenderbufferStorage", renderbuffer_storage);
    add("_glRenderbufferStorageOES", renderbuffer_storage);
    const auto framebuffer_renderbuffer = [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto target = call.argument(0);
        const auto attachment = call.argument(1);
        const auto renderbuffer_target = call.argument(2);
        const auto name = call.argument(3);
        if (target != gles_abi::framebuffer ||
            (attachment != gles_abi::color_attachment0 &&
                attachment != gles_abi::depth_attachment &&
                attachment != gles_abi::stencil_attachment) ||
            (name != 0U && renderbuffer_target != gles_abi::renderbuffer)) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        if (context->bound_framebuffer == 0U) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        auto framebuffer =
            context->framebuffers.find(context->bound_framebuffer);
        if (framebuffer == context->framebuffers.end()) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (name != 0U && !context->renderbuffers.contains(name)) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (attachment == gles_abi::depth_attachment) {
            framebuffer->second.depth_renderbuffer = name;
            return;
        }
        if (attachment == gles_abi::stencil_attachment) {
            framebuffer->second.stencil_renderbuffer = name;
            return;
        }
        if (name == 0U) {
            framebuffer->second.color_renderbuffer = 0U;
            framebuffer->second.color_texture = 0U;
            framebuffer->second.color_texture_target = 0U;
            return;
        }
        const auto renderbuffer = context->renderbuffers.find(name);
        if (renderbuffer == context->renderbuffers.end()) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        framebuffer->second.color_renderbuffer = name;
        framebuffer->second.color_texture =
            context->renderbuffers.at(name).color_texture;
        framebuffer->second.color_texture_target = gles_abi::renderbuffer;
    };
    add("_glFramebufferRenderbuffer", framebuffer_renderbuffer);
    add("_glFramebufferRenderbufferOES", framebuffer_renderbuffer);
    const auto get_renderbuffer_parameter = [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto target = call.argument(0);
        const auto parameter = call.argument(1);
        const auto output = call.argument(2);
        const auto renderbuffer =
            context->renderbuffers.find(context->bound_renderbuffer);
        if (target != gles_abi::renderbuffer ||
            renderbuffer == context->renderbuffers.end() || output == 0U) {
            set_gl_error(call, target != gles_abi::renderbuffer
                                   ? gles_abi::invalid_enum
                                   : gles_abi::invalid_operation);
            return;
        }
        std::uint32_t value { };
        switch (parameter) {
        case gl_renderbuffer_width:
            value = renderbuffer->second.width;
            break;
        case gl_renderbuffer_height:
            value = renderbuffer->second.height;
            break;
        case gl_renderbuffer_internal_format:
        case gl_renderbuffer_color_format:
            value = renderbuffer->second.internal_format;
            break;
        default:
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        if (!call.memory().write32(output, value))
            set_gl_error(call, gles_abi::invalid_value);
    };
    add("_glGetRenderbufferParameteriv", get_renderbuffer_parameter);
    add("_glGetRenderbufferParameterivOES", get_renderbuffer_parameter);
    const auto delete_renderbuffers = [this](UserlandHleCall& call) {
        if (!flush_program_draws(call))
            return;
        auto* context = current_context(call);
        const auto count = static_cast<std::int32_t>(call.argument(0));
        const auto input = call.argument(1);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (count < 0 || (count != 0 && input == 0U)) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        for (std::int32_t index = 0; index < count; ++index) {
            const auto name = call.memory().read32(
                input + static_cast<std::uint32_t>(index) * 4U);
            if (!name) {
                set_gl_error(call, gles_abi::invalid_value);
                return;
            }
            const auto renderbuffer = context->renderbuffers.find(*name);
            if (renderbuffer == context->renderbuffers.end())
                continue;
            for (auto& [framebuffer_name, framebuffer] :
                context->framebuffers) {
                static_cast<void>(framebuffer_name);
                if (framebuffer.depth_renderbuffer == *name)
                    framebuffer.depth_renderbuffer = 0U;
                if (framebuffer.stencil_renderbuffer == *name)
                    framebuffer.stencil_renderbuffer = 0U;
                if (framebuffer.color_renderbuffer == *name) {
                    framebuffer.color_renderbuffer = 0U;
                    framebuffer.color_texture = 0U;
                    framebuffer.color_texture_target = 0U;
                }
            }
            if (context->bound_renderbuffer == *name)
                context->bound_renderbuffer = 0U;
            if (renderbuffer->second.color_texture != 0U)
                resources_.erase_texture(renderbuffer->second.color_texture);
            context->renderbuffers.erase(renderbuffer);
        }
    };
    add("_glDeleteRenderbuffers", delete_renderbuffers);
    add("_glDeleteRenderbuffersOES", delete_renderbuffers);
}

} // namespace ilemu
