// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "graphics/gles_program_state.hpp"
#include "graphics/gles_renderer.hpp"

#include <array>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ilemu {

// Guest memory and object names are decoded before crossing this boundary.
// The host compiler executes GLSL; no host API is exposed to the guest kernel.
struct GlesProgramDraw {
    struct Attribute {
        std::uint32_t location;
        std::uint32_t size { }, type { }, stride { };
        bool normalized { };
        std::array<float, 4> value { };
        // Buffer storage remains alive for the synchronous draw. Client arrays
        // are copied from guest memory; host code never dereferences it.
        std::span<const std::byte> buffer;
        std::vector<std::byte> client;
    };
    std::uint32_t program { };
    std::uint32_t vertex_count { };
    std::array<std::uint32_t, gles_abi::programmable_texture_unit_count>
        textures { };
    std::vector<Attribute> attributes;
    std::vector<std::uint16_t> indices;
    const GlesProgramState::Program* state { };
    bool depth_test { };
    bool depth_write { true };
    std::uint32_t depth_function { 0x0201U };
    float depth_clear { 1.0F };
    std::uint64_t depth_generation { };
};

class GlesProgramRenderer {
public:
    struct Limits {
        std::uint32_t vertex_attributes { };
        std::uint32_t vertex_uniform_vectors { };
        std::uint32_t fragment_uniform_vectors { };
        std::uint32_t varying_vectors { };
        std::uint32_t vertex_texture_units { };
        std::uint32_t combined_texture_units { };
    };
    struct LinkResult {
        bool linked { };
        std::map<std::string, std::uint32_t, std::less<>> attributes;
        std::string log;
        std::vector<GlesProgramState::ActiveVariable> active_attributes;
        std::vector<GlesProgramState::ActiveVariable> active_uniforms;
    };
    virtual ~GlesProgramRenderer() = default;
    [[nodiscard]] virtual std::optional<Limits> limits() = 0;
    [[nodiscard]] virtual LinkResult link(std::uint32_t program,
        std::string_view vertex, std::string_view fragment,
        const std::map<std::string, std::uint32_t, std::less<>>& bindings) = 0;
    virtual void erase(std::uint32_t program) = 0;
    [[nodiscard]] virtual bool contains(std::uint32_t program) const = 0;
    [[nodiscard]] virtual std::optional<std::array<std::int32_t, 3>> precision(
        std::uint32_t shader_type, std::uint32_t precision_type) = 0;
    // An empty input frame continues the target's queued GPU contents.
    [[nodiscard]] virtual bool draw(DisplayFrame& frame,
        GlesRenderTargetKey target, const GlesProgramDraw& program,
        std::uint32_t mode, const GlesRasterState& state) = 0;
    // Publish queued GPU contents at a guest-visible synchronization boundary.
    [[nodiscard]] virtual bool readback(
        DisplayFrame& frame, GlesRenderTargetKey target) = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
};

using GlesProgramRendererFactory = std::unique_ptr<GlesProgramRenderer> (*)(
    std::string*);
void configure_gles_program_renderer_factory(
    GlesProgramRendererFactory factory);
[[nodiscard]] std::unique_ptr<GlesProgramRenderer> create_gles_program_renderer(
    std::string* error);

} // namespace ilemu
