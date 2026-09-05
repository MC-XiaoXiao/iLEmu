#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ilemu {

// Owns the API-visible GLES2 shader/program objects. Shader execution remains
// in the existing renderer; this store only preserves the declarations and
// values needed to adapt conventional programmable inputs to that pipeline.
class GlesProgramState {
public:
    struct Shader {
        std::uint32_t type { };
        std::string source;
        bool compiled { };
        bool delete_pending { };
    };

    struct Uniform {
        std::string name;
        std::array<float, 16> values { };
        std::size_t value_count { };
        std::optional<std::int32_t> integer;
    };

    struct Program {
        std::vector<std::uint32_t> shaders;
        std::map<std::string, std::uint32_t, std::less<>> attributes;
        std::map<std::string, std::int32_t, std::less<>> uniform_locations;
        std::map<std::int32_t, Uniform> uniforms;
        bool linked { };
        bool delete_pending { };
    };

    void reset();

    [[nodiscard]] std::uint32_t create_shader(std::uint32_t type);
    [[nodiscard]] Shader* shader(std::uint32_t name);
    [[nodiscard]] const Shader* shader(std::uint32_t name) const;
    void delete_shader(std::uint32_t name);

    [[nodiscard]] std::uint32_t create_program();
    [[nodiscard]] Program* program(std::uint32_t name);
    [[nodiscard]] const Program* program(std::uint32_t name) const;
    void delete_program(std::uint32_t name);

    [[nodiscard]] bool attach_shader(
        std::uint32_t program, std::uint32_t shader);
    [[nodiscard]] bool bind_attribute(
        std::uint32_t program, std::uint32_t index, std::string name);
    [[nodiscard]] bool link(std::uint32_t program);
    [[nodiscard]] std::optional<std::uint32_t> attribute(
        std::uint32_t program, std::string_view name) const;
    [[nodiscard]] std::int32_t uniform_location(
        std::uint32_t program, std::string_view name);
    [[nodiscard]] bool set_uniform(std::uint32_t program, std::int32_t location,
        std::span<const float> values);
    [[nodiscard]] bool set_uniform(
        std::uint32_t program, std::int32_t location, std::int32_t value);
    [[nodiscard]] const Uniform* uniform(
        std::uint32_t program, std::string_view name) const;
    [[nodiscard]] std::string_view shader_source(
        std::uint32_t program, std::uint32_t type) const;

private:
    void collect_deleted_shaders();

    std::map<std::uint32_t, Shader> shaders_;
    std::map<std::uint32_t, Program> programs_;
    std::uint32_t next_shader_ { 1U };
    std::uint32_t next_program_ { 1U };
};

} // namespace ilemu
