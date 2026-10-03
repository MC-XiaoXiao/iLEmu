// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include "foundation/output.hpp"
#include "foundation/userland_hle.hpp"
#include "kernel/opengles_hle.hpp"

#include <algorithm>
#include <limits>

namespace ilemu {
namespace {
    constexpr std::string_view opengles_image {
        "/OpenGLES.framework/OpenGLES"
    };
    bool write_empty_log(UserlandHleCall& call, std::uint32_t capacity,
        std::uint32_t length, std::uint32_t output)
    {
        if (length != 0U && !call.memory().write32(length, 0U))
            return false;
        return capacity == 0U || output == 0U ||
               call.memory().write8(output, 0U);
    }
}
bool OpenGlesHle::ensure_program_renderer(
    UserlandHleCall& call, std::string* error)
{
    if (!program_renderer_) {
        program_renderer_ = create_gles_program_renderer(error);
        if (program_renderer_)
            call.output().marker("[gles] GLSL executor=" +
                                 std::string { program_renderer_->name() });
    }
    return program_renderer_ != nullptr;
}

std::optional<std::uint32_t> OpenGlesHle::program_integer_limit(
    UserlandHleCall& call, std::uint32_t parameter)
{
    if (!program_limits_ && ensure_program_renderer(call))
        program_limits_ = program_renderer_->limits();
    if (!program_limits_)
        return std::nullopt;
    const auto& limits = *program_limits_;
    constexpr auto uniform_vectors = static_cast<std::uint32_t>(
        GlesProgramState::maximum_uniform_components / 4U);
    constexpr auto texture_units = static_cast<std::uint32_t>(
        gles_abi::programmable_texture_unit_count);
    switch (parameter) {
    case gles_abi::maximum_vertex_attributes_query:
        return std::min(limits.vertex_attributes,
            static_cast<std::uint32_t>(gles_abi::maximum_vertex_attributes));
    case gles_abi::maximum_vertex_uniform_vectors:
        return std::min(limits.vertex_uniform_vectors, uniform_vectors);
    case gles_abi::maximum_fragment_uniform_vectors:
        return std::min(limits.fragment_uniform_vectors, uniform_vectors);
    case gles_abi::maximum_varying_vectors:
        return limits.varying_vectors;
    case gles_abi::maximum_vertex_texture_image_units:
        return std::min(limits.vertex_texture_units, texture_units);
    case gles_abi::maximum_combined_texture_image_units:
        return std::min(limits.combined_texture_units, texture_units);
    default:
        return std::nullopt;
    }
}

void OpenGlesHle::register_program_queries(UserlandHleRegistry& registry)
{
    const auto add = [&](std::string symbol,
                         UserlandHleRegistry::Handler handler) {
        registry.register_function(std::string { opengles_image },
            std::move(symbol), std::move(handler));
    };
    add("_glGetShaderiv", [this](UserlandHleCall& call) {
        const auto* shader = programs_.shader(call.argument(0));
        const auto output = call.argument(2);
        if (shader == nullptr || output == 0U) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        std::uint32_t value { };
        switch (call.argument(1)) {
        case gles_abi::shader_type:
            value = shader->type;
            break;
        case gles_abi::compile_status:
            value = shader->compiled ? 1U : 0U;
            break;
        case gles_abi::delete_status:
            value = shader->delete_pending ? 1U : 0U;
            break;
        case gles_abi::info_log_length:
            value = 1U;
            break;
        case gles_abi::shader_source_length:
            value = static_cast<std::uint32_t>(shader->source.size() + 1U);
            break;
        default:
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        if (!call.memory().write32(output, value))
            set_gl_error(call, gles_abi::invalid_value);
    });
    add("_glGetProgramiv", [this](UserlandHleCall& call) {
        const auto* program = programs_.program(call.argument(0));
        const auto output = call.argument(2);
        if (program == nullptr || output == 0U) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        std::uint32_t value { };
        switch (call.argument(1)) {
        case gles_abi::link_status:
        case gles_abi::validate_status:
            value = program->linked ? 1U : 0U;
            break;
        case gles_abi::delete_status:
            value = program->delete_pending ? 1U : 0U;
            break;
        case gles_abi::info_log_length:
            value = static_cast<std::uint32_t>(program->info_log.size() + 1U);
            break;
        case gles_abi::attached_shaders:
            value = static_cast<std::uint32_t>(program->shaders.size());
            break;
        case gles_abi::active_uniforms:
            value = static_cast<std::uint32_t>(
                program->native_execution ? program->active_uniforms.size()
                                          : program->uniforms.size());
            break;
        case gles_abi::active_uniform_max_length:
            for (const auto& variable : program->active_uniforms)
                value = std::max(value,
                    static_cast<std::uint32_t>(variable.name.size() + 1U));
            break;
        case gles_abi::active_attribute_max_length:
            for (const auto& variable : program->active_attributes)
                value = std::max(value,
                    static_cast<std::uint32_t>(variable.name.size() + 1U));
            break;
        case gles_abi::active_attributes:
            value = static_cast<std::uint32_t>(
                program->native_execution ? program->active_attributes.size()
                                          : program->attributes.size());
            break;
        default:
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        if (!call.memory().write32(output, value))
            set_gl_error(call, gles_abi::invalid_value);
    });
    const auto get_info_log = [this](UserlandHleCall& call) {
        const auto exists =
            call.symbol() == "_glGetShaderInfoLog"
                ? programs_.shader(call.argument(0)) != nullptr
                : programs_.program(call.argument(0)) != nullptr;
        if (!exists) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        if (call.symbol() == "_glGetProgramInfoLog") {
            const auto& log = programs_.program(call.argument(0))->info_log;
            const auto capacity = static_cast<std::int32_t>(call.argument(1));
            if (capacity < 0) {
                set_gl_error(call, gles_abi::invalid_value);
                return;
            }
            const auto count = capacity > 0
                                   ? std::min(log.size(),
                                         static_cast<std::size_t>(capacity - 1))
                                   : 0U;
            if ((call.argument(2) != 0U &&
                    !call.memory().write32(
                        call.argument(2), static_cast<std::uint32_t>(count))) ||
                (capacity > 0 &&
                    (!call.memory().copy_in(call.argument(3),
                         std::as_bytes(std::span { log.data(), count })) ||
                        !call.memory().write8(
                            call.argument(3) +
                                static_cast<std::uint32_t>(count),
                            0U))))
                set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        if (!write_empty_log(
                call, call.argument(1), call.argument(2), call.argument(3))) {
            set_gl_error(call, gles_abi::invalid_value);
        }
    };
    add("_glGetShaderInfoLog", get_info_log);
    add("_glGetProgramInfoLog", get_info_log);

    const auto get_active = [this](UserlandHleCall& call, bool uniform) {
        const auto* program = programs_.program(call.argument(0));
        const auto index = call.argument(1);
        const auto capacity = static_cast<std::int32_t>(call.argument(2));
        if (!program) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        const auto& variables =
            uniform ? program->active_uniforms : program->active_attributes;
        if (capacity < 0 || index >= variables.size()) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        const auto& variable = variables[index];
        const auto count = capacity > 0
                               ? std::min(variable.name.size(),
                                     static_cast<std::size_t>(capacity - 1))
                               : 0U;
        const auto write = [&](std::uint32_t address, std::uint32_t value) {
            return address == 0U || call.memory().write32(address, value);
        };
        if (!write(call.argument(3), static_cast<std::uint32_t>(count)) ||
            !write(call.argument(4), variable.size) ||
            !write(call.argument(5), variable.type)) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        if (capacity > 0) {
            auto name = variable.name.substr(0, count);
            name.push_back('\0');
            if (call.argument(6) == 0U ||
                !call.memory().copy_in(
                    call.argument(6), std::as_bytes(std::span { name })))
                set_gl_error(call, gles_abi::invalid_value);
        }
    };
    add("_glGetActiveUniform",
        [get_active](UserlandHleCall& call) { get_active(call, true); });
    add("_glGetActiveAttrib",
        [get_active](UserlandHleCall& call) { get_active(call, false); });
    add("_glGetShaderPrecisionFormat", [this](UserlandHleCall& call) {
        const auto shader = call.argument(0), type = call.argument(1);
        if ((shader != gles_abi::vertex_shader &&
                shader != gles_abi::fragment_shader) ||
            type < 0x8df0U || type > 0x8df5U) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        const auto result = ensure_program_renderer(call)
                                ? program_renderer_->precision(shader, type)
                                : std::nullopt;
        if (!result) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto output = call.argument(2);
        if (output == 0U ||
            output > std::numeric_limits<std::uint32_t>::max() - 4U ||
            call.argument(3) == 0U ||
            !call.memory().write32(
                output, static_cast<std::uint32_t>((*result)[0])) ||
            !call.memory().write32(
                output + 4U, static_cast<std::uint32_t>((*result)[1])) ||
            !call.memory().write32(
                call.argument(3), static_cast<std::uint32_t>((*result)[2])))
            set_gl_error(call, gles_abi::invalid_value);
    });
}
} // namespace ilemu
