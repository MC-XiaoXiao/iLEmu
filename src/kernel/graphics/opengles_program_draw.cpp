// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "foundation/output.hpp"
#include "foundation/userland_hle.hpp"
#include "graphics/surface_store.hpp"
#include "kernel/opengles_hle.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

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

void OpenGlesHle::retire_thread(std::size_t processor)
{
    // Cancel only the observer. Accepted host work retains its snapshots and
    // host resource references through execution; late results have no copyout.
    if (sync_lease_ && sync_lease_->owner == processor)
        sync_lease_->cancelled = true;
    if (program_renderer_)
        program_renderer_->retire_issuer(processor);
    threads_.erase(processor);
}

void OpenGlesHle::collect_program_draw_errors()
{
    if (!program_renderer_)
        return;
    for (const auto issuer : program_renderer_->collect_errors()) {
        auto& error = threads_[issuer].gl_error;
        if (error == gles_abi::no_error)
            error = gles_abi::invalid_operation;
    }
}

bool OpenGlesHle::flush_program_draws(UserlandHleCall& call)
{
    if (program_flush_failed_)
        return false;
    if (!pending_program_target_)
        return true;
    auto& target = *pending_program_target_;
    const auto read =
        program_renderer_ &&
        program_renderer_->readback(target.frame, target.binding.key);
    collect_program_draw_errors();
    if (!read) {
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

bool OpenGlesHle::program_draw_needs_flush(const RenderTargetBinding& binding,
    std::span<const std::uint32_t> textures) const
{
    if (!pending_program_target_)
        return false;
    const auto& pending = pending_program_target_->binding;
    if (pending.key != binding.key)
        return true;
    for (const auto name : textures) {
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
}

void OpenGlesHle::execute_program_draw(UserlandHleCall& call,
    const RenderTargetBinding& binding, DisplayFrame frame,
    const GlesProgramDraw& draw, std::uint32_t mode,
    const GlesRasterState& state, bool indexed)
{
    // Host initialization/restoration was completed before draw preparation.
    if (!program_renderer_ || !program_renderer_->initialized() ||
        !program_renderer_->contains(draw.program)) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
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
    }
    collect_program_draw_errors();
    std::shared_ptr<GlesProgramWorker::PreparedDraw> prepared;
    std::size_t bytes = 0;
    try {
        prepared = program_renderer_->prepare_draw(frame, binding.key, draw,
            mode, state, call.cpu().processor_id(), bytes,
            !pending_program_target_, *surface_store_);
    } catch (const std::bad_alloc&) {
        set_gl_error(call, gles_abi::out_of_memory);
        return;
    } catch (const std::length_error&) {
        set_gl_error(call, gles_abi::out_of_memory);
        return;
    } catch (const std::exception&) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    if (!prepared) {
        // No immutable payload or command was accepted. Release temporary
        // attribute views/frame data on return; preparation may safely restart
        // from frozen scalar ABI after credit becomes available.
        const auto lease = program_wait(call);
        call.suspend_until(
            [bytes] { return gles_draw_preparation_capacity(bytes); },
            [this, lease, indexed](UserlandHleCall& resumed) {
                lease->consume();
                this->draw(resumed, indexed);
            });
        return;
    }
    const DisplayGeometry dimensions { frame.width, frame.height };
    if (!program_renderer_->inputs_ready(prepared)) {
        prepare_program_inputs(call, binding, dimensions, std::move(prepared));
        return;
    }
    submit_program_draw(call, binding, dimensions, std::move(prepared));
}

void OpenGlesHle::prepare_program_inputs(UserlandHleCall& call,
    const RenderTargetBinding& binding, DisplayGeometry dimensions,
    std::shared_ptr<GlesProgramWorker::PreparedDraw> prepared)
{
    const auto lease = program_wait(call);
    if (!program_renderer_ || !binding_live(binding)) {
        lease->consume();
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    std::shared_ptr<GlesWorkResult> result;
    try {
        result = program_renderer_->enqueue_input_readback(
            prepared, renderer_, binding.host_surface);
    } catch (const std::bad_alloc&) {
        lease->consume();
        set_gl_error(call, gles_abi::out_of_memory);
        return;
    } catch (...) {
        lease->consume();
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    if (!result) {
        call.suspend_until(
            [this, prepared] {
                return !program_renderer_ ||
                       program_renderer_->draw_capacity(prepared);
            },
            [this, binding, dimensions, prepared](UserlandHleCall& resumed) {
                prepare_program_inputs(resumed, binding, dimensions, prepared);
            });
        return;
    }
    lease->result = result;
    call.suspend_until([result] { return result->ready(); },
        [this, binding, dimensions, prepared, lease](UserlandHleCall& resumed) {
            try {
                if (!program_renderer_ || !binding_live(binding))
                    throw std::invalid_argument { "stale GLES input binding" };
                program_renderer_->complete_inputs(prepared, resources_);
                lease->result.reset();
            } catch (const std::bad_alloc&) {
                lease->consume();
                set_gl_error(resumed, gles_abi::out_of_memory);
                return;
            } catch (...) {
                lease->consume();
                set_gl_error(resumed, gles_abi::invalid_operation);
                return;
            }
            // Keep the process graphics gate until draw admission. The packet
            // already owns attributes/uniforms/textures; do not replay draw().
            submit_program_draw(resumed, binding, dimensions, prepared);
        },
        result->sequence);
}

void OpenGlesHle::submit_program_draw(UserlandHleCall& call,
    const RenderTargetBinding& binding, DisplayGeometry dimensions,
    std::shared_ptr<GlesProgramWorker::PreparedDraw> prepared)
{
    const auto release_admission = [&] {
        if (sync_lease_ && !sync_lease_->consumed &&
            sync_lease_->owner == call.cpu().processor_id())
            sync_lease_->consume();
    };
    if (!program_renderer_ || !binding_live(binding)) {
        release_admission();
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    std::shared_ptr<GlesWorkResult> result;
    try {
        result = program_renderer_->enqueue_draw(prepared);
    } catch (const std::bad_alloc&) {
        release_admission();
        set_gl_error(call, gles_abi::out_of_memory);
        return;
    } catch (const std::exception&) {
        release_admission();
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    if (!result) {
        static_cast<void>(program_wait(call));
        call.suspend_until(
            [this, prepared] {
                return !program_renderer_ ||
                       program_renderer_->draw_capacity(prepared);
            },
            [this, binding, dimensions, prepared](UserlandHleCall& resumed) {
                submit_program_draw(resumed, binding, dimensions, prepared);
            });
        return;
    }
    performance_counters().record_draw();
    if (!pending_program_target_)
        pending_program_target_ = PendingProgramTarget { binding,
            DisplayFrame { dimensions.width, dimensions.height, 0, { } } };
    release_admission();
}

} // namespace ilemu
