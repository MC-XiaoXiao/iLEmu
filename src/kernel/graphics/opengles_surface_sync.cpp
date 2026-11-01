// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/opengles_hle.hpp"

#include "surface_guest_import.hpp"

#include "graphics/surface_store.hpp"

#include <utility>

namespace ilemu {

void OpenGlesHle::create_pixmap_surface(UserlandHleCall& call, std::uint32_t id)
try {
    const auto backing = surface_store_->find(id);
    constexpr std::size_t limit = 64U * 1024U * 1024U;
    const auto count =
        backing ? static_cast<std::uint64_t>(backing->width) * backing->height
                : 0;
    if (!count || count > limit / sizeof(std::uint32_t)) {
        publish_created_surface(call, std::nullopt);
        return;
    }
    const auto owner = renderer_owner_;
    const auto lease = program_wait(call);
    SurfaceGuestImport::read(
        call, surface_store_, id,
        [this, backing = *backing, owner, lease](UserlandHleCall& resumed,
            std::optional<std::vector<std::uint32_t>> pixels) {
            try {
                lease->consume();
                if (!pixels || owner != renderer_owner_) {
                    publish_created_surface(resumed, std::nullopt);
                    return;
                }
                SurfaceState state;
                state.backing_identifier = backing.id;
                state.width = backing.width;
                state.height = backing.height;
                state.pixels = std::move(*pixels);
                if (const auto surface =
                        surface_store_->host_surface(backing.id))
                    state.backing_cpu_generation = surface->cpu_generation();
                publish_created_surface(resumed, std::move(state));
            } catch (...) {
                lease->consume();
                publish_created_surface(resumed, std::nullopt);
            }
        },
        lease);
} catch (...) {
    if (sync_lease_)
        sync_lease_->cancelled = true;
    publish_created_surface(call, std::nullopt);
}

bool OpenGlesHle::prepare_pixmap_surface(
    UserlandHleCall& call, const UserlandHleRegistry::Handler& handler)
{
    const auto symbol = call.symbol();
    if (symbol != "_glClear" && symbol != "_glDrawArrays" &&
        symbol != "_glDrawElements" && symbol != "_glReadPixels" &&
        symbol != "_glFinish" && symbol != "_glFlush")
        return false;
    const auto* context = current_context(call);
    if (!context || context->bound_framebuffer)
        return false;
    auto* state = current_pixmap_surface(call);
    if (!state)
        return false;
    const auto surface =
        surface_store_->host_surface(*state->backing_identifier);
    if (!surface)
        return false;
    const auto cpu_generation = surface->cpu_generation();
    if (cpu_generation <= state->backing_cpu_generation)
        return false;
    if (surface->gpu_generation() > cpu_generation) {
        state->backing_cpu_generation = cpu_generation;
        return false;
    }
    reload_surface_async(call, thread(call).draw_surface,
        [this, handler](UserlandHleCall& resumed, bool succeeded) {
            if (succeeded)
                await_graphics_call(resumed, handler);
            else
                set_gl_error(resumed, gles_abi::invalid_operation);
        });
    return true;
}

void OpenGlesHle::reload_surface_async(UserlandHleCall& call,
    std::uint32_t surface,
    std::function<void(UserlandHleCall&, bool)> completed)
try {
    const auto found = surfaces_.find(surface);
    if (found == surfaces_.end()) {
        completed(call, false);
        return;
    }
    auto& state = found->second;
    if (!state.backing_identifier) {
        completed(call, true);
        return;
    }
    const auto id = *state.backing_identifier;
    const auto backing = surface_store_->find(id);
    const auto host_surface = surface_store_->host_surface(id);
    if (!backing || !host_surface) {
        completed(call, false);
        return;
    }
    if (host_surface->gpu_generation() > host_surface->cpu_generation()) {
        state.width = backing->width;
        state.height = backing->height;
        state.backing_cpu_generation = host_surface->cpu_generation();
        state.refreshed_textures.clear();
        state.dirty = false;
        completed(call, true);
        return;
    }
    const auto owner = renderer_owner_;
    auto binding = RenderTargetBinding { };
    binding.key = render_target_key(surface);
    binding.backing_identifier = id;
    binding.pixmap_surface = &state;
    binding.surface_identity = state.identity;
    binding.host_surface = host_surface;
    const auto lease = program_wait(call);
    SurfaceGuestImport::read(
        call, surface_store_, id,
        [this, binding, owner, lease, completed](UserlandHleCall& resumed,
            std::optional<std::vector<std::uint32_t>> pixels) {
            try {
                lease->consume();
                if (!pixels || owner != renderer_owner_ ||
                    !binding_live(binding)) {
                    completed(resumed, false);
                    return;
                }
                auto lifetime = surface_store_->acquire_transport_lease(
                    *binding.backing_identifier);
                if (!lifetime) {
                    completed(resumed, false);
                    return;
                }
                auto& state = *binding.pixmap_surface;
                const auto descriptor = binding.host_surface->descriptor();
                state.width = descriptor.width;
                state.height = descriptor.height;
                state.pixels = std::move(*pixels);
                state.backing_cpu_generation =
                    binding.host_surface->cpu_generation();
                state.refreshed_textures.clear();
                state.dirty = false;
                submit_host_work_async(
                    resumed, 0,
                    [renderer = renderer_, key = binding.key, lifetime](
                        DisplayFrame&) {
                        renderer->invalidate(key);
                        return true;
                    },
                    [this, binding, owner, completed](UserlandHleCall& done,
                        const std::shared_ptr<GlesWorkResult>& result) {
                        completed(done, result && result->succeeded() &&
                                            owner == renderer_owner_ &&
                                            binding_live(binding));
                    });
            } catch (...) {
                lease->consume();
                completed(resumed, false);
            }
        },
        lease);
} catch (...) {
    if (sync_lease_)
        sync_lease_->cancelled = true;
    completed(call, false);
}

} // namespace ilemu
