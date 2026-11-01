// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/opengles_hle.hpp"

#include "foundation/address_space.hpp"
#include "graphics/surface_store.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace ilemu {

struct OpenGlesHle::NativeClear {
    struct Work {
        std::shared_ptr<void> credit, lifetime;
        std::shared_ptr<GlesRenderer> renderer;
        std::shared_ptr<HostSurface> target;
        HostRectangle rectangle;
        std::uint32_t color { };
        bool filled { }, fallback_ready { };

        bool execute()
        {
            const auto encoder = renderer->create_command_encoder();
            filled = encoder && encoder->fill(target, rectangle, color);
            if (!filled) {
                // Preserve the existing CPU fallback without a driver call
                // from its core writeback. A stale CPU shadow still fails.
                renderer->invalidate(target->key());
                fallback_ready = true;
            }
            return filled;
        }
    };
    std::shared_ptr<Work> work;
    RenderTargetBinding binding;
    std::uint64_t owner { };
    AddressSpace::MappingObservation mapping;
    std::uint32_t base { }, size { };
    std::shared_ptr<const GlesWorkCompletion> previous_writer;
    std::shared_ptr<GlesWorkResult> result;
    std::shared_ptr<SyncLease> lease;
};

void OpenGlesHle::clear_native_async(UserlandHleCall& call,
    const RenderTargetBinding& binding, HostRectangle rectangle,
    std::uint32_t color)
try {
    auto pending = std::make_shared<NativeClear>();
    pending->work = std::make_shared<NativeClear::Work>();
    const auto work = pending->work;
    pending->binding = binding;
    pending->owner = renderer_owner_;
    work->renderer = renderer_;
    work->target = binding.host_surface;
    work->rectangle = rectangle;
    work->color = color;
    if (binding.backing_identifier) {
        work->lifetime = surface_store_->acquire_transport_lease(
            *binding.backing_identifier);
        const auto backing = surface_store_->find(*binding.backing_identifier);
        if (!backing)
            throw std::runtime_error { "missing native clear backing" };
        pending->base = backing->base;
        pending->size = backing->allocation_size;
        pending->mapping = call.memory().observe_mapping(
            pending->base, pending->size);
    } else {
        work->lifetime = resources_.texture(binding.framebuffer_texture)
                             ->levels.at(0U).native_lifetime;
    }
    if (!work->lifetime)
        throw std::runtime_error { "missing native clear owner" };
    auto complete = [this, work, binding, owner = pending->owner] {
        if (work->filled && owner == renderer_owner_ && binding_live(binding) &&
            binding.framebuffer_texture)
            resources_.update_texture_render_target_generation(
                binding.framebuffer_texture, 0U);
        // Do not make the next private writable lookup COW solely because
        // this completed call still holds its temporary native owner.
        work->lifetime.reset();
    };
    pending->lease = program_wait(call);
    pending->lease->complete_core = std::move(complete);
    await_native_clear(call, std::move(pending));
} catch (const std::bad_alloc&) {
    set_gl_error(call, gles_abi::out_of_memory);
} catch (...) {
    set_gl_error(call, gles_abi::invalid_operation);
}

void OpenGlesHle::await_native_clear(
    UserlandHleCall& call, std::shared_ptr<NativeClear> pending)
try {
    if (pending->owner != renderer_owner_ || !binding_live(pending->binding))
        throw std::runtime_error { "stale native clear target" };
    const auto work = pending->work;
    if (!pending->result) {
        constexpr auto bytes = sizeof(NativeClear) + sizeof(NativeClear::Work);
        if (!work->credit) {
            work->credit = reserve_gles_host_preparation(bytes);
            if (!work->credit) {
                call.suspend_until(
                    [] { return gles_host_preparation_capacity(bytes); },
                    [this, pending](UserlandHleCall& resumed) {
                        await_native_clear(resumed, pending);
                    });
                return;
            }
        }
        pending->previous_writer = work->target->writer();
        pending->result = enqueue_gles_surface_write(work->target,
            [work](DisplayFrame&) { return work->execute(); });
        if (!pending->result) {
            call.suspend_until(
                [target = work->target] {
                    const auto writer = target->writer();
                    return (!writer || writer->ready()) &&
                           gles_host_work_capacity(0);
                },
                [this, pending](UserlandHleCall& resumed) {
                    await_native_clear(resumed, pending);
                });
            return;
        }
        pending->lease->result = pending->result;
    }
    const auto result = pending->result;
    call.suspend_until([result] { return result->ready(); },
        [this, pending](UserlandHleCall& resumed) {
            try {
                if (pending->owner != renderer_owner_ ||
                    !binding_live(pending->binding))
                    throw std::runtime_error { "native clear target replaced" };
                const auto completed = pending->work;
                if (!pending->result->succeeded()) {
                    if (!completed->fallback_ready)
                        throw std::runtime_error { "native clear failed" };
                    if (pending->binding.backing_identifier) {
                        const auto backing = surface_store_->find(
                            *pending->binding.backing_identifier);
                        if (!backing || backing->base != pending->base ||
                            backing->allocation_size != pending->size ||
                            !pending->mapping.valid())
                            throw std::runtime_error { "clear output replaced" };
                    }
                    auto snapshot = completed->target->snapshot_cpu(
                        PerfCpuMapReason::SoftwareFallback);
                    if (snapshot.gpu_generation > snapshot.cpu_generation)
                        throw std::runtime_error { "stale clear CPU shadow" };
                    auto& frame = snapshot.frame;
                    const auto rectangle = completed->rectangle;
                    for (std::uint32_t row = 0; row < rectangle.height; ++row) {
                        const auto begin = frame.pixels.begin() +
                            (static_cast<std::size_t>(rectangle.y) + row) *
                                frame.width + rectangle.x;
                        std::fill_n(begin, rectangle.width, completed->color);
                    }
                    // The failed native write admits no successor writer.
                    // Recover its valid CPU image in this one core boundary.
                    completed->target->abandon_write(pending->result->completion(),
                        std::move(pending->previous_writer));
                    if (!commit_render_target(resumed, pending->binding,
                            std::move(frame)))
                        throw std::runtime_error { "clear CPU commit failed" };
                }
            } catch (const std::bad_alloc&) {
                set_gl_error(resumed, gles_abi::out_of_memory);
            } catch (...) {
                set_gl_error(resumed, gles_abi::invalid_operation);
            }
            pending->lease->consume();
        }, result->sequence());
} catch (const std::bad_alloc&) {
    pending->lease->cancelled = true;
    set_gl_error(call, gles_abi::out_of_memory);
} catch (...) {
    pending->lease->cancelled = true;
    set_gl_error(call, gles_abi::invalid_operation);
}

} // namespace ilemu
