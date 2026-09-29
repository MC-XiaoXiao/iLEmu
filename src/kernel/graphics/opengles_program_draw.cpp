// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include "foundation/output.hpp"
#include "foundation/userland_hle.hpp"
#include "graphics/surface_store.hpp"
#include "kernel/opengles_hle.hpp"

#include <algorithm>
#include <limits>

namespace ilemu {

bool OpenGlesHle::prepare_program_attributes(UserlandHleCall& call,
    const ContextState& context, std::uint32_t first,
    GlesProgramDraw& draw) const
{
    auto vertex_count = draw.vertex_count;
    std::vector<bool> referenced;
    if (!draw.indices.empty()) {
        const auto [minimum, maximum] =
            std::minmax_element(draw.indices.begin(), draw.indices.end());
        first = *minimum;
        vertex_count = static_cast<std::uint32_t>(*maximum) - first + 1U;
        referenced.resize(vertex_count);
        for (auto& index : draw.indices) {
            index = static_cast<std::uint16_t>(index - first);
            referenced[index] = true;
        }
    }
    for (const auto& [name, location] : draw.state->attributes) {
        static_cast<void>(name);
        if (location >= context.current_generic_attributes.size())
            return false;
        auto& attribute = draw.attributes.emplace_back();
        attribute.location = location;
        attribute.value = context.current_generic_attributes[location];
        const auto found = context.generic_arrays.find(location);
        if (found == context.generic_arrays.end() || !found->second.enabled)
            continue;
        const auto& array = found->second;
        const auto component_size =
            array.type == gles_abi::float_type || array.type == gles_abi::fixed
                ? 4U
            : array.type == gles_abi::short_type ||
                    array.type == gles_abi::unsigned_short
                ? 2U
            : array.type == gles_abi::byte ||
                    array.type == gles_abi::unsigned_byte
                ? 1U
                : 0U;
        if (!component_size || !array.size || array.size > 4U)
            return false;
        const auto element_size = array.size * component_size;
        const auto stride = array.stride ? array.stride : element_size;
        const auto start = static_cast<std::uint64_t>(array.pointer) +
                           static_cast<std::uint64_t>(first) * stride;
        const auto extent =
            static_cast<std::uint64_t>(vertex_count - 1U) * stride +
            element_size;
        attribute.size = array.size;
        attribute.type = array.type;
        attribute.normalized = array.normalized;
        if (array.buffer) {
            const auto* buffer = resources_.buffer(array.buffer);
            if (!buffer || start > buffer->bytes.size() ||
                extent > buffer->bytes.size() - start)
                return false;
            attribute.stride = stride;
            attribute.buffer = std::span { buffer->bytes }.subspan(
                static_cast<std::size_t>(start),
                static_cast<std::size_t>(extent));
        } else {
            if (start > std::numeric_limits<std::uint32_t>::max() ||
                extent > (std::uint64_t { 1 } << 32U) - start)
                return false;
            attribute.client.resize(
                static_cast<std::size_t>(vertex_count) * element_size);
            // Read only referenced vertices. Sparse client arrays can cross
            // unmapped gaps, and padding need not be readable guest memory.
            for (std::uint32_t index = 0; index < vertex_count; ++index) {
                if (!referenced.empty() && !referenced[index])
                    continue;
                if (!call.memory().copy_out(
                        static_cast<std::uint32_t>(
                            start + static_cast<std::uint64_t>(index) * stride),
                        std::span { attribute.client }.subspan(
                            static_cast<std::size_t>(index) * element_size,
                            element_size)))
                    return false;
            }
        }
    }
    return true;
}

bool OpenGlesHle::flush_program_draws(UserlandHleCall& call)
{
    if (!pending_program_target_)
        return true;
    auto& target = *pending_program_target_;
    if (!program_renderer_ ||
        !program_renderer_->readback(target.frame, target.binding.key)) {
        set_gl_error(call, gles_abi::invalid_operation);
        return false;
    }
    renderer_->invalidate(target.binding.key);
    if (!commit_render_target(call, target.binding, std::move(target.frame))) {
        set_gl_error(call, gles_abi::invalid_operation);
        return false;
    }
    pending_program_target_.reset();
    return true;
}

void OpenGlesHle::execute_program_draw(UserlandHleCall& call,
    const RenderTargetBinding& binding, DisplayFrame frame,
    const GlesProgramDraw& draw, std::uint32_t mode,
    const GlesRasterState& state)
{
    // Forked guest contexts inherit declarations, but never host EGL objects.
    if (!program_renderer_) {
        std::string error;
        program_renderer_ = create_gles_program_renderer(&error);
        if (!program_renderer_) {
            call.output().marker(
                "[gles] cannot restore GLSL executor: " + error);
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
    }
    if (!program_renderer_->contains(draw.program)) {
        const auto linked = program_renderer_->link(draw.program,
            programs_.shader_source(draw.program, gles_abi::vertex_shader),
            programs_.shader_source(draw.program, gles_abi::fragment_shader),
            draw.state->attributes);
        if (!linked.linked) {
            call.output().marker(
                "[gles] cannot restore GLSL program: " + linked.log);
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
    }
    const auto samples_pending = [&] {
        if (!pending_program_target_)
            return false;
        const auto& pending = pending_program_target_->binding;
        for (const auto name : draw.textures) {
            if (name == 0U)
                continue;
            if (name == pending.framebuffer_texture)
                return true;
            const auto* texture = resources_.texture(name);
            if (texture && pending.backing_identifier)
                for (const auto& [level, image] : texture->levels) {
                    static_cast<void>(level);
                    if (image.surface_id == pending.backing_identifier)
                        return true;
                }
        }
        return false;
    };
    if (pending_program_target_ &&
        (pending_program_target_->binding.key != binding.key ||
            samples_pending()) &&
        !flush_program_draws(call))
        return;
    for (auto texture : draw.textures) {
        if (texture == 0U)
            continue;
        const auto error = resources_.refresh_surface_texture(
            call.memory(), texture, *surface_store_);
        if (error != gles_abi::no_error) {
            set_gl_error(call, error);
            return;
        }
    }
    if (pending_program_target_) {
        frame.pixels.clear();
    } else {
        // Switching targets can publish a preceding render into the same
        // backing texture. Reacquire the frame after that synchronization.
        const auto refreshed = render_target(call, binding);
        if (!refreshed) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        frame = *refreshed;
        if (binding.host_surface) {
            if (!renderer_->map_cpu(*binding.host_surface, true)) {
                set_gl_error(call, gles_abi::invalid_operation);
                return;
            }
            auto mapping = binding.host_surface->map_cpu(false);
            frame = mapping.frame();
        } else if (!renderer_->synchronize(frame, binding.key)) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
    }
    if (!resources_.materialize_surface_textures(*renderer_) ||
        !program_renderer_->draw(frame, binding.key, draw, mode, state)) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    performance_counters().record_draw();
    if (!pending_program_target_) {
        frame.pixels.clear();
        pending_program_target_ =
            PendingProgramTarget { binding, std::move(frame) };
    }
}

} // namespace ilemu
