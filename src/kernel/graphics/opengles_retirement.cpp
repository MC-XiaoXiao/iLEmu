// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include "kernel/opengles_hle.hpp"

#include <limits>

namespace ilemu {

void OpenGlesHle::commit_texture_deletion(std::span<const std::uint32_t> names)
{
    for (const auto name : names) {
        resources_.erase_texture(name);
        for (auto& [context_name, context] : contexts_) {
            static_cast<void>(context_name);
            for (auto& unit : context.texture_units) {
                if (unit.bound_texture_2d == name)
                    unit.bound_texture_2d = 0;
                if (unit.bound_texture_rectangle == name)
                    unit.bound_texture_rectangle = 0;
            }
            for (auto& [framebuffer_name, framebuffer] : context.framebuffers) {
                static_cast<void>(framebuffer_name);
                if (framebuffer.color_texture == name)
                    framebuffer.color_texture = 0;
            }
        }
    }
}

void OpenGlesHle::delete_names(UserlandHleCall& call)
{
    if (program_flush_failed_)
        return;
    const auto signed_count = static_cast<std::int32_t>(call.argument(0));
    const auto input = call.argument(1);
    if (!current_context(call)) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    if (signed_count < 0 || (signed_count != 0 && input == 0)) {
        set_gl_error(call, gles_abi::invalid_value);
        return;
    }
    const auto count = static_cast<std::uint32_t>(signed_count);
    const auto read_name = [&](std::uint32_t index) {
        const auto address = static_cast<std::uint64_t>(input) +
                             static_cast<std::uint64_t>(index) * 4U;
        return address <= std::numeric_limits<std::uint32_t>::max()
                   ? call.memory().read32(static_cast<std::uint32_t>(address))
                   : std::optional<std::uint32_t> { };
    };
    if (call.symbol() != "_glDeleteTextures") {
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto name = read_name(index);
            if (!name) {
                set_gl_error(call, gles_abi::invalid_value);
                break;
            }
            resources_.erase_buffer(*name);
            for (auto& [context_name, context] : contexts_) {
                static_cast<void>(context_name);
                if (context.bound_array_buffer == *name)
                    context.bound_array_buffer = 0;
                if (context.bound_element_array_buffer == *name)
                    context.bound_element_array_buffer = 0;
            }
        }
        return;
    }

    constexpr std::size_t limit = 64U * 1024U * 1024U;
    const auto name_bytes = static_cast<std::uint64_t>(count) * 4U;
    if (name_bytes > limit) {
        set_gl_error(call, gles_abi::out_of_memory);
        return;
    }
    // Native images carry GlesTextureTarget ownership. Erasing the logical
    // name posts physical retirement only after its last host/fork user exits.
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto name = read_name(index);
        if (!name) {
            set_gl_error(call, gles_abi::invalid_value);
            break;
        }
        commit_texture_deletion(std::span { &*name, std::size_t { 1 } });
    }
}

} // namespace ilemu
