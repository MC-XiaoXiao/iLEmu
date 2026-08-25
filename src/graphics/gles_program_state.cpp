#include "ilemu/gles_program_state.hpp"

#include "ilemu/gles_abi.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace ilemu {

void GlesProgramState::reset()
{
    shaders_.clear();
    programs_.clear();
    next_shader_ = 1U;
    next_program_ = 1U;
}

std::uint32_t GlesProgramState::create_shader(std::uint32_t type)
{
    const auto name = next_shader_++;
    Shader value;
    value.type = type;
    shaders_.emplace(name, std::move(value));
    return name;
}

GlesProgramState::Shader* GlesProgramState::shader(std::uint32_t name)
{
    const auto found = shaders_.find(name);
    return found == shaders_.end() ? nullptr : &found->second;
}

const GlesProgramState::Shader* GlesProgramState::shader(
    std::uint32_t name) const
{
    const auto found = shaders_.find(name);
    return found == shaders_.end() ? nullptr : &found->second;
}

void GlesProgramState::delete_shader(std::uint32_t name)
{
    auto* value = shader(name);
    if (value == nullptr)
        return;
    value->delete_pending = true;
    collect_deleted_shaders();
}

std::uint32_t GlesProgramState::create_program()
{
    const auto name = next_program_++;
    programs_.try_emplace(name);
    return name;
}

GlesProgramState::Program* GlesProgramState::program(std::uint32_t name)
{
    const auto found = programs_.find(name);
    return found == programs_.end() ? nullptr : &found->second;
}

const GlesProgramState::Program* GlesProgramState::program(
    std::uint32_t name) const
{
    const auto found = programs_.find(name);
    return found == programs_.end() ? nullptr : &found->second;
}

void GlesProgramState::delete_program(std::uint32_t name)
{
    programs_.erase(name);
    collect_deleted_shaders();
}

bool GlesProgramState::attach_shader(
    std::uint32_t program_name, std::uint32_t shader_name)
{
    auto* program_value = program(program_name);
    if (program_value == nullptr || shader(shader_name) == nullptr)
        return false;
    if (std::find(program_value->shaders.begin(), program_value->shaders.end(),
            shader_name) == program_value->shaders.end()) {
        program_value->shaders.push_back(shader_name);
    }
    program_value->linked = false;
    return true;
}

bool GlesProgramState::bind_attribute(
    std::uint32_t program_name, std::uint32_t index, std::string name)
{
    auto* program_value = program(program_name);
    if (program_value == nullptr || name.empty())
        return false;
    program_value->attributes.insert_or_assign(std::move(name), index);
    return true;
}

bool GlesProgramState::link(std::uint32_t program_name)
{
    auto* program_value = program(program_name);
    if (program_value == nullptr)
        return false;
    const auto has_compiled = [&](std::uint32_t type) {
        return std::any_of(program_value->shaders.begin(),
            program_value->shaders.end(), [&](std::uint32_t shader_name) {
                const auto* value = shader(shader_name);
                return value != nullptr && value->type == type &&
                       value->compiled;
            });
    };
    program_value->linked = has_compiled(gles_abi::vertex_shader) &&
                            has_compiled(gles_abi::fragment_shader);
    return program_value->linked;
}

std::optional<std::uint32_t> GlesProgramState::attribute(
    std::uint32_t program_name, std::string_view name) const
{
    const auto* program_value = program(program_name);
    if (program_value == nullptr || !program_value->linked)
        return std::nullopt;
    const auto found = program_value->attributes.find(name);
    return found == program_value->attributes.end()
               ? std::nullopt
               : std::optional<std::uint32_t> { found->second };
}

std::int32_t GlesProgramState::uniform_location(
    std::uint32_t program_name, std::string_view name)
{
    auto* program_value = program(program_name);
    if (program_value == nullptr || !program_value->linked || name.empty())
        return -1;
    const auto declared = std::any_of(program_value->shaders.begin(),
        program_value->shaders.end(), [&](std::uint32_t shader_name) {
            const auto* value = shader(shader_name);
            return value != nullptr &&
                   value->source.find(name) != std::string::npos;
        });
    if (!declared)
        return -1;
    if (const auto found = program_value->uniform_locations.find(name);
        found != program_value->uniform_locations.end()) {
        return found->second;
    }
    if (program_value->uniform_locations.size() >=
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return -1;
    }
    const auto location =
        static_cast<std::int32_t>(program_value->uniform_locations.size());
    auto owned_name = std::string { name };
    program_value->uniform_locations.emplace(owned_name, location);
    Uniform uniform;
    uniform.name = std::move(owned_name);
    program_value->uniforms.emplace(location, std::move(uniform));
    return location;
}

bool GlesProgramState::set_uniform(std::uint32_t program_name,
    std::int32_t location, std::span<const float> values)
{
    auto* program_value = program(program_name);
    if (program_value == nullptr || !program_value->linked)
        return false;
    const auto found = program_value->uniforms.find(location);
    if (found == program_value->uniforms.end() ||
        values.size() > found->second.values.size()) {
        return false;
    }
    std::copy(values.begin(), values.end(), found->second.values.begin());
    found->second.value_count = values.size();
    found->second.integer.reset();
    return true;
}

bool GlesProgramState::set_uniform(
    std::uint32_t program_name, std::int32_t location, std::int32_t value)
{
    auto* program_value = program(program_name);
    if (program_value == nullptr || !program_value->linked)
        return false;
    const auto found = program_value->uniforms.find(location);
    if (found == program_value->uniforms.end())
        return false;
    found->second.integer = value;
    found->second.value_count = 0U;
    return true;
}

const GlesProgramState::Uniform* GlesProgramState::uniform(
    std::uint32_t program_name, std::string_view name) const
{
    const auto* program_value = program(program_name);
    if (program_value == nullptr)
        return nullptr;
    const auto location = program_value->uniform_locations.find(name);
    if (location == program_value->uniform_locations.end())
        return nullptr;
    const auto value = program_value->uniforms.find(location->second);
    return value == program_value->uniforms.end() ? nullptr : &value->second;
}

std::string_view GlesProgramState::shader_source(
    std::uint32_t program_name, std::uint32_t type) const
{
    const auto* program_value = program(program_name);
    if (program_value == nullptr)
        return { };
    for (const auto shader_name : program_value->shaders) {
        const auto* value = shader(shader_name);
        if (value != nullptr && value->type == type)
            return value->source;
    }
    return { };
}

void GlesProgramState::collect_deleted_shaders()
{
    for (auto candidate = shaders_.begin(); candidate != shaders_.end();) {
        const auto attached = std::any_of(
            programs_.begin(), programs_.end(), [&](const auto& program_entry) {
                return std::find(program_entry.second.shaders.begin(),
                           program_entry.second.shaders.end(),
                           candidate->first) !=
                       program_entry.second.shaders.end();
            });
        if (candidate->second.delete_pending && !attached) {
            candidate = shaders_.erase(candidate);
        } else {
            ++candidate;
        }
    }
}

} // namespace ilemu
