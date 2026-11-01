// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include "foundation/userland_hle.hpp"
#include "graphics/surface_store.hpp"
#include "guest_eagl_window.hpp"
#include "kernel/opengles_hle.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

namespace ilemu {
namespace {
    constexpr std::string_view opengles_image {
        "/OpenGLES.framework/OpenGLES"
    };
    constexpr std::uint32_t gl_renderbuffer_width = 0x8d42;
    constexpr std::uint32_t gl_renderbuffer_height = 0x8d43;
    constexpr std::uint32_t gl_renderbuffer_internal_format = 0x8d44;
    constexpr std::uint32_t gl_renderbuffer_color_format = 0x8e10;
    class DrawableWindowRelease
        : public std::enable_shared_from_this<DrawableWindowRelease> {
    public:
        explicit DrawableWindowRelease(std::vector<GuestEaglWindow> windows)
            : windows_ { std::move(windows) }
        {
        }

        void run(UserlandHleCall& call)
        {
            if (next_ == windows_.size()) {
                call.set_return(0U);
                return;
            }
            const auto window = windows_[next_++];
            window.release(
                call, [self = shared_from_this()](
                          UserlandHleCall& released) { self->run(released); });
        }

    private:
        std::vector<GuestEaglWindow> windows_;
        std::size_t next_ { };
    };

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

void OpenGlesHle::present_eagl_window_async(UserlandHleCall& call,
    const ContextState& context, const RenderTargetBinding& binding)
try {
    const auto& renderbuffer =
        context.renderbuffers.at(context.bound_renderbuffer);
    const auto window = GuestEaglWindow::open(call, renderbuffer.native_window);
    const auto transport =
        surface_store_->acquire_transport_lease(*binding.backing_identifier);
    if (!window || !transport) {
        call.set_return(0U);
        return;
    }
    const auto renderbuffer_name = context.bound_renderbuffer;
    // This resolver stays on the core across both the host wait
    // and the firmware's window callbacks. No context pointer is
    // retained through either scheduling boundary.
    const auto live =
        [this, context_id = context.identity, buffer_id = renderbuffer.identity,
            renderbuffer_name, binding, address = window->address(),
            owner = renderer_owner_, transport]() -> ContextState* {
        static_cast<void>(transport);
        if (owner != renderer_owner_ || !binding_live(binding))
            return nullptr;
        const auto found =
            std::ranges::find_if(contexts_, [&](const auto& entry) {
                return entry.second.identity == context_id;
            });
        if (found == contexts_.end())
            return nullptr;
        auto& state = found->second;
        const auto buffer = state.renderbuffers.find(renderbuffer_name);
        if (buffer == state.renderbuffers.end() ||
            buffer->second.identity != buffer_id ||
            buffer->second.native_window != address ||
            buffer->second.drawable_surface_id != binding.backing_identifier)
            return nullptr;
        return &state;
    };
    submit_renderer_async(call, binding.key, false, transport,
        [this, live, renderbuffer_name, window = *window](
            UserlandHleCall& submitted, bool succeeded) {
            if (!succeeded || !live()) {
                submitted.set_return(0U);
                return;
            }
            window.present(submitted, [this, live, renderbuffer_name,
                                          address = window.address()](
                                          UserlandHleCall& completed,
                                          bool presented,
                                          std::uint32_t next_surface) {
                if (!presented) {
                    completed.set_return(0U);
                    return;
                }
                const auto finish = [this, live, renderbuffer_name, address,
                                        next_surface](UserlandHleCall& ready) {
                    auto* current = live();
                    if (program_flush_failed_ || !current) {
                        ready.set_return(0U);
                        return;
                    }
                    if (!attach_eagl_window_surface(ready, *current,
                            renderbuffer_name, address, next_surface)) {
                        // The firmware already presented this frame.
                        // Preserve the existing private next-buffer
                        // fallback without changing its return value.
                        auto& buffer =
                            current->renderbuffers.at(renderbuffer_name);
                        if (ensure_renderbuffer_storage(*current,
                                renderbuffer_name, buffer.width, buffer.height,
                                gles_abi::bgra_apple) == gles_abi::no_error)
                            buffer.display_oriented = true;
                    }
                    ready.set_return(1U);
                };
                // A firmware callback can yield to another guest
                // thread. Respect its accepted GL store work before
                // rotating the original renderbuffer's attachment.
                enter_graphics_call(
                    completed, [this, finish](UserlandHleCall& ready) {
                        if (pending_program_target_)
                            await_program_flush(ready, finish);
                        else
                            finish(ready);
                    });
            });
        });
} catch (...) {
    call.set_return(0U);
}

void OpenGlesHle::complete_drawable_storage(UserlandHleCall& call,
    ResourceIdentity context_id, ResourceIdentity renderbuffer_id,
    std::uint32_t name, DisplayGeometry pixels,
    std::optional<DisplayViewport> viewport, std::uint32_t window,
    std::uint32_t surface, std::uint64_t publication)
{
    const auto retry = [this, context_id, renderbuffer_id, name, pixels,
                           viewport, window, surface,
                           publication](UserlandHleCall& resumed) {
        complete_drawable_storage(resumed, context_id, renderbuffer_id, name,
            pixels, viewport, window, surface, publication);
    };
    // Guest geometry/window queries can yield before this callback. Another
    // thread may have submitted GL work or deleted/replaced either object.
    enter_graphics_call(call, [this, context_id, renderbuffer_id, name, pixels,
                                  viewport, window, surface, publication,
                                  retry](UserlandHleCall& ready) {
        if (program_flush_failed_) {
            ready.set_return(0U);
            return;
        }
        if (pending_program_target_) {
            await_program_flush(ready, retry);
            return;
        }
        const auto found =
            std::ranges::find_if(contexts_, [&](const auto& entry) {
                return entry.second.identity == context_id;
            });
        if (found == contexts_.end()) {
            ready.set_return(0U);
            return;
        }
        auto& context = found->second;
        const auto buffer = context.renderbuffers.find(name);
        const auto identifier = core_surface_identifier(ready, surface);
        const auto backing =
            identifier ? surface_store_->find(*identifier) : std::nullopt;
        if (buffer == context.renderbuffers.end() ||
            buffer->second.identity != renderbuffer_id ||
            (publication &&
                (!backing ||
                    backing->provenance.publication_sequence != publication))) {
            ready.set_return(0U);
            return;
        }
        const auto attached =
            surface != 0U &&
            attach_eagl_window_surface(ready, context, name, window, surface);
        const auto error =
            attached ? gles_abi::no_error
                     : ensure_renderbuffer_storage(context, name, pixels.width,
                           pixels.height, gles_abi::bgra_apple);
        if (error == gles_abi::no_error) {
            buffer->second.display_oriented = true;
            buffer->second.drawable_viewport = viewport;
        }
        ready.set_return(error == gles_abi::no_error ? 1U : 0U);
    });
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
        if (program_flush_failed_)
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
        if (program_flush_failed_)
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
        if (program_flush_failed_)
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
        std::vector<GuestEaglWindow> windows;
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
            if (const auto window = GuestEaglWindow::open(
                    call, renderbuffer->second.native_window))
                windows.push_back(*window);
            if (context->bound_renderbuffer == *name)
                context->bound_renderbuffer = 0U;
            if (renderbuffer->second.color_texture != 0U)
                resources_.erase_texture(renderbuffer->second.color_texture);
            context->renderbuffers.erase(renderbuffer);
        }
        // QuartzCore owns the drawable allocation. Release its outstanding
        // surface before a deleted name is reused for a new drawable.
        if (!windows.empty())
            std::make_shared<DrawableWindowRelease>(std::move(windows))
                ->run(call);
    };
    add("_glDeleteRenderbuffers", delete_renderbuffers);
    add("_glDeleteRenderbuffersOES", delete_renderbuffers);
}

} // namespace ilemu
