// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include "foundation/userland_hle.hpp"
#include "kernel/opengles_hle.hpp"

#include <limits>
#include <new>

namespace ilemu {

void OpenGlesHle::register_vertex_arrays(UserlandHleRegistry& registry)
{
    const auto add = [&registry](std::string symbol,
                         UserlandHleRegistry::Handler handler) {
        registry.register_function("/OpenGLES.framework/OpenGLES",
            std::move(symbol), std::move(handler));
    };
    const auto validate_range = [this](UserlandHleCall& call) {
        const auto count = static_cast<std::int32_t>(call.argument(0));
        const auto address = call.argument(1);
        const auto bytes = static_cast<std::int64_t>(count) * 4;
        if (count < 0 ||
            (count != 0 && (address == 0 ||
                               address + bytes - 1 >
                                   std::numeric_limits<std::uint32_t>::max() ||
                               !call.memory().mapped(address,
                                   static_cast<std::size_t>(bytes))))) {
            set_gl_error(call, gles_abi::invalid_value);
            return 0;
        }
        return count;
    };
    add("_glGenVertexArraysOES", [this, validate_range](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto count = validate_range(call);
        const auto output = call.argument(1);
        for (std::int32_t index = 0; index < count; ++index) {
            auto& name = context->next_vertex_array;
            while (name == 0 || context->vertex_array_objects.contains(name))
                ++name;
            try {
                context->vertex_array_objects.try_emplace(name, std::nullopt);
            } catch (const std::bad_alloc&) {
                set_gl_error(call, gles_abi::out_of_memory);
                return;
            }
            if (!call.memory().write32(
                    output + static_cast<std::uint32_t>(index) * 4U, name)) {
                context->vertex_array_objects.erase(name);
                set_gl_error(call, gles_abi::invalid_value);
                return;
            }
            ++name;
        }
    });
    add("_glBindVertexArrayOES", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto name = call.argument(0);
        if (name != 0) {
            const auto object = context->vertex_array_objects.find(name);
            if (object == context->vertex_array_objects.end()) {
                set_gl_error(call, gles_abi::invalid_operation);
                return;
            }
            if (!object->second)
                object->second.emplace();
        }
        context->bound_vertex_array = name;
    });
    add("_glDeleteVertexArraysOES",
        [this, validate_range](UserlandHleCall& call) {
            auto* context = current_context(call);
            if (context == nullptr) {
                set_gl_error(call, gles_abi::invalid_operation);
                return;
            }
            const auto count = validate_range(call);
            const auto input = call.argument(1);
            for (std::int32_t index = 0; index < count; ++index) {
                const auto name = call.memory().read32(
                    input + static_cast<std::uint32_t>(index) * 4U);
                if (!name) {
                    set_gl_error(call, gles_abi::invalid_value);
                    return;
                }
                if (context->bound_vertex_array == *name)
                    context->bound_vertex_array = 0;
                context->vertex_array_objects.erase(*name);
            }
        });
    add("_glIsVertexArrayOES", [this](UserlandHleCall& call) {
        const auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            call.set_return(0U);
            return;
        }
        const auto object =
            context->vertex_array_objects.find(call.argument(0));
        call.set_return(object != context->vertex_array_objects.end() &&
                                object->second.has_value()
                            ? 1U
                            : 0U);
    });
}

} // namespace ilemu
