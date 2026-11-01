// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/mbx2d_hle.hpp"

#include "foundation/cpu.hpp"
#include "graphics/gles_renderer.hpp"

#include <stdexcept>
#include <utility>

namespace ilemu {
struct Mbx2dHle::NativeFill {
    struct Work {
        std::shared_ptr<void> credit;
        std::shared_ptr<SurfaceTransportLease> transport;
        std::shared_ptr<GlesRenderer> renderer;
        std::shared_ptr<CommandEncoder> encoder;
        std::shared_ptr<HostSurface> target;
        HostRectangle rectangle;
        std::uint32_t color;
        HostCompositeMode mode;
        std::uint8_t alpha;

        bool execute() const
        {
            return encoder &&
                   encoder->fill(target, rectangle, color, mode, alpha);
        }
    };
    std::shared_ptr<Work> work;
    ResolvedSurface destination;
    AddressSpace::MappingObservation mapping;
    std::shared_ptr<GraphicsCallLease> lease;
    std::shared_ptr<const GlesWorkCompletion> previous_writer;
    std::uint64_t owner { };
    bool context_api { };
};

void Mbx2dHle::fill_native(UserlandHleCall& call, bool context_api,
    ResolvedSurface destination, HostRectangle rectangle,
    std::uint32_t color, HostCompositeMode mode, std::uint8_t alpha)
try {
    auto pending = std::make_shared<NativeFill>();
    pending->destination = std::move(destination);
    const auto& target = pending->destination;
    pending->mapping = call.memory().observe_mapping(
        target.backing->base, target.backing->allocation_size);
    pending->owner = renderer_owner_;
    pending->context_api = context_api;
    pending->work = std::make_shared<NativeFill::Work>();
    const auto work = pending->work;
    work->renderer = host_graphics_;
    work->encoder = command_encoder_;
    work->target = target.host_surface;
    work->rectangle = rectangle;
    work->color = color;
    work->mode = mode;
    work->alpha = alpha;
    work->transport =
        surface_store_->acquire_transport_lease(target.core_surface_id);
    if (!work->transport)
        throw std::runtime_error { "missing MBX native target owner" };
    pending->lease = std::make_shared<GraphicsCallLease>();
    pending->lease->owner = call.cpu().processor_id();
    sync_lease_ = pending->lease;
    await_native_fill(call, std::move(pending));
} catch (...) {
    call.set_return(mbx2d_abi::failure);
}

void Mbx2dHle::await_native_fill(
    UserlandHleCall& call, std::shared_ptr<NativeFill> pending)
try {
    const auto* bindings = select_state(call, pending->context_api);
    if (pending->owner != renderer_owner_ || pending->lease->cancelled ||
        pending->lease->consumed || !bindings ||
        !surface_binding_live(pending->destination, resolve(bindings->destination),
            pending->mapping))
        throw std::runtime_error { "stale MBX native target" };
    const auto gate = pending->lease;
    const auto work = pending->work;
    if (!gate->result) {
        constexpr auto bytes = sizeof(NativeFill) + sizeof(NativeFill::Work) +
                               8U * sizeof(void*);
        if (!work->credit) {
            work->credit = reserve_gles_host_preparation(bytes);
            if (!work->credit) {
                call.suspend_until(
                    [] { return gles_host_preparation_capacity(bytes); },
                    [this, pending](UserlandHleCall& resumed) {
                        await_native_fill(resumed, pending);
                    });
                return;
            }
        }
        auto dependency = work->target->writer();
        if (dependency && dependency->ready() && !dependency->succeeded())
            throw std::runtime_error { "failed MBX native writer" };
        pending->previous_writer = dependency;
        if (!dependency || dependency->ready())
            dependency = work->target->reader();
        if (dependency && !dependency->ready()) {
            call.suspend_until([dependency] { return dependency->ready(); },
                [this, pending](UserlandHleCall& resumed) {
                    await_native_fill(resumed, pending);
                }, dependency->sequence());
            return;
        }
        gate->result = enqueue_gles_surface_write(work->target,
            [work](DisplayFrame&) { return work->execute(); });
        if (!gate->result) {
            call.suspend_until([] { return gles_host_work_capacity(0); },
                [this, pending](UserlandHleCall& resumed) {
                    await_native_fill(resumed, pending);
                });
            return;
        }
    }
    const auto result = gate->result;
    call.suspend_until([result] { return result->ready(); },
        [this, pending](UserlandHleCall& resumed) {
            const auto* state = select_state(resumed, pending->context_api);
            const auto live = pending->owner == renderer_owner_ && state &&
                              surface_binding_live(pending->destination,
                                  resolve(state->destination), pending->mapping);
            const auto lease = pending->lease;
            if (!live) {
                lease->consume();
                resumed.set_return(mbx2d_abi::failure);
                return;
            }
            const auto succeeded = lease->result->succeeded();
            if (!succeeded) {
                // Keep the original software fallback, with the original ABI
                // and store gate. The failed native command is never retried.
                pending->work->target->abandon_write(
                    lease->result->completion(), pending->previous_writer);
                lease->consume();
                blit_color(resumed, pending->context_api, true);
                return;
            }
            lease->consume();
            resumed.set_return(mbx2d_abi::success);
        }, result->sequence());
} catch (...) {
    pending->lease->cancelled = true;
    if (pending->lease->ready())
        pending->lease->consume();
    call.set_return(mbx2d_abi::failure);
}
} // namespace ilemu
